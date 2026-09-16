/* kache - opening the backing file: choosing a geometry that fits, laying
 * out a new store, adopting an existing one, and recovering after a crash. */
#include <stddef.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "store/alloc.h"
#include "store/map.h"
#include "store/shard.h"
#include "util/clk.h"
#include "util/util.h"

/* biggest arena we can address with 32 bit block sizes */
#define ARENA_MAX 0xfffffff0ull

static u64
csum(const Hdr *h)
{
	const u8 *p = (const u8 *)h;
	size_t i, n = offsetof(Hdr, csum);
	u64 v = 0xcbf29ce484222325ull;

	for (i = 0; i < n; i++) {
		v ^= p[i];
		v *= 0x100000001b3ull;
	}
	return v;
}

/* Exclusive use of the store is enforced with an advisory lock.  A
 * restart commonly overlaps its predecessor's teardown, so the lock is
 * retried briefly instead of failing on the first contended attempt. */
static int
grab(int fd)
{
	struct timespec nap = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
	int i;

	for (i = 0; i < 300; i++) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0)
			return 0;
		if (errno != EWOULDBLOCK && errno != EINTR)
			return -1;
		nanosleep(&nap, NULL);
	}
	return -1;
}

static u64
item_max(const MapCfg *c)
{
	return (u64)REC_HDR + c->maxkey + c->maxval + ST_GRAIN;
}

/* Split the file into nshards equal slices, each an index followed by an
 * arena.  nshards is adjusted until every slice can hold a sensible
 * number of maximum sized items. */
static int
geometry(const MapCfg *c, u64 *nshards_out, u64 *slice_out, u64 *data_off,
         u64 *nbuckets_out, u64 *arena_out)
{
	u64 want = c->shards ? c->shards :
	    MIN(c->size / (CFG_ARENA_ITEMS * item_max(c)), c->size >> 20);
	u64 n = ppow2(want);

	if (n < CFG_SHARDS_MIN)
		n = CFG_SHARDS_MIN;
	if (n > CFG_SHARDS_MAX)
		n = CFG_SHARDS_MAX;

	for (;;) {
		u64 off = ALIGNUP(ST_HDR_SZ + n * ST_SHARD_SZ, 64ull);
		u64 slice, nb, arena;

		if (c->size <= off)
			goto shrink;
		slice = ALIGNDN((c->size - off) / n, 64ull);
		/* An item of avg bytes needs 1/LOAD_LIMIT of a bucket to
		 * stay addressable, so solve slice = nb*(16 + avg*load)
		 * rather than assuming one item per bucket. */
		nb = ppow2(slice / (sizeof(Bucket) +
		     ((u64)c->avg_item * CFG_LOAD_LIMIT >> 8)));
		if (nb < 64)
			nb = 64;
		if (nb * sizeof(Bucket) >= slice)
			goto shrink;
		arena = ALIGNDN(slice - nb * sizeof(Bucket), (u64)ST_GRAIN);

		if (arena > ARENA_MAX) {
			if (n < CFG_SHARDS_MAX) {
				n <<= 1;
				continue;
			}
			arena = ALIGNDN(ARENA_MAX, (u64)ST_GRAIN);
		}
		if (arena < 2 * item_max(c) + ST_GRAIN)
			goto shrink;

		*nshards_out = n;
		*slice_out = slice;
		*data_off = off;
		*nbuckets_out = nb;
		*arena_out = arena;
		return 0;
shrink:
		if (n <= CFG_SHARDS_MIN)
			return -1;
		n >>= 1;
	}
}

static void
layout(Map *m, u64 data_off, u64 slice, u64 nbuckets, u64 arena)
{
	u32 i;

	for (i = 0; i < m->nshards; i++) {
		Shard *s = &m->shards[i];

		memset(s, 0, sizeof(*s));
		lock_init(&s->lock);
		s->nbuckets = (u32)nbuckets;
		s->buckets_off = data_off + (u64)i * slice;
		s->arena_off = s->buckets_off + nbuckets * sizeof(Bucket);
		s->arena_size = arena;
		s->rng = m->seed ^ (0x9e3779b97f4a7c15ull * (i + 1));
		shd_format(m, s, s->rng);
	}
}

static int
create(Map *m, const MapCfg *c)
{
	u64 n, slice, data_off, nbuckets, arena;
	Hdr *h;

	if (geometry(c, &n, &slice, &data_off, &nbuckets, &arena) < 0) {
		warn("%s: file of %llu bytes is too small for items of %u bytes",
		     c->path, (unsigned long long)c->size, c->maxval);
		return -1;
	}
	if (ftruncate(m->fd, 0) < 0 || ftruncate(m->fd, (off_t)c->size) < 0) {
		warn("%s: cannot size the backing file:", c->path);
		return -1;
	}
	/* Reserve the blocks now: a sparse mapping that runs out of disk
	 * later would take the process down with SIGBUS. */
	if (posix_fallocate(m->fd, 0, (off_t)c->size) != 0)
		warn("%s: cannot preallocate, the file stays sparse", c->path);

	m->size = c->size;
	if ((m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                    m->fd, 0)) == MAP_FAILED) {
		warn("%s: mmap:", c->path);
		m->base = NULL;
		return -1;
	}
	memset(m->base, 0, ST_HDR_SZ);

	h = m->hdr = (Hdr *)m->base;
	h->magic = ST_MAGIC;
	h->version = ST_VERSION;
	h->hdrsz = ST_HDR_SZ;
	h->filesz = c->size;
	h->seed = entropy64();
	h->nshards = (u32)n;
	h->shard_shift = (u32)__builtin_ctzll(n);
	h->shards_off = ST_HDR_SZ;
	h->slice = slice;
	h->maxkey = c->maxkey;
	h->maxval = c->maxval;
	h->shardsz = ST_SHARD_SZ;
	h->flags = 0;
	h->created = clk_read_ms();
	h->opened = h->created;

	m->shards = (Shard *)(m->base + h->shards_off);
	m->nshards = h->nshards;
	m->shard_shift = h->shard_shift;
	m->seed = h->seed;
	m->maxkey = h->maxkey;
	m->maxval = h->maxval;

	layout(m, data_off, slice, nbuckets, arena);
	h->csum = csum(h);
	info("created %s: %llu MiB, %u shards, %llu buckets and %llu KiB arena each",
	     c->path, (unsigned long long)(c->size >> 20), h->nshards,
	     (unsigned long long)nbuckets, (unsigned long long)(arena >> 10));
	return 0;
}

static int
adopt(Map *m, const MapCfg *c, u64 fsize)
{
	Hdr probe, *h;
	u32 i;
	u64 recovered = 0;

	if (fsize < ST_HDR_SZ)
		return -1;
	if ((m->base = mmap(NULL, fsize, PROT_READ | PROT_WRITE, MAP_SHARED,
	                    m->fd, 0)) == MAP_FAILED) {
		warn("%s: mmap:", c->path);
		m->base = NULL;
		return -1;
	}
	m->size = fsize;
	h = (Hdr *)m->base;
	probe = *h;
	if (probe.magic != ST_MAGIC || probe.version != ST_VERSION ||
	    probe.hdrsz != ST_HDR_SZ || probe.shardsz != ST_SHARD_SZ ||
	    probe.filesz != fsize || probe.nshards == 0 ||
	    probe.nshards != (1u << probe.shard_shift) ||
	    probe.shard_shift == 0 || probe.csum != csum(&probe)) {
		warn("%s: not a kache store, or written by another version; "
		     "use -n to start over", c->path);
		return -1;
	}

	m->hdr = h;
	m->shards = (Shard *)(m->base + h->shards_off);
	m->nshards = h->nshards;
	m->shard_shift = h->shard_shift;
	m->seed = h->seed;
	m->maxkey = h->maxkey;
	m->maxval = h->maxval;

	if (c->size != fsize)
		info("%s: keeping the existing geometry of %llu MiB and %u shards",
		     c->path, (unsigned long long)(fsize >> 20), m->nshards);

	/* A crash can leave a shard lock held by a thread that no longer
	 * exists, so the locks are always reset on open. */
	for (i = 0; i < m->nshards; i++)
		lock_init(&m->shards[i].lock);

	if (!(h->flags & ST_CLEAN)) {
		u64 now = clk_read_ms();
		u64 broken = 0;

		info("%s: unclean shutdown, rebuilding the index", c->path);
		for (i = 0; i < m->nshards; i++) {
			if (shd_rebuild(m, &m->shards[i], now) < 0)
				broken++;
			else
				recovered += m->shards[i].count;
		}
		if (broken)
			warn("%s: %llu of %u shards were damaged and reset",
			     c->path, (unsigned long long)broken, m->nshards);
		info("%s: recovered %llu records", c->path,
		     (unsigned long long)recovered);
	}
	h->opened = clk_read_ms();
	return 0;
}

int
map_open(Map *m, const MapCfg *cfg)
{
	struct stat st;
	MapCfg c = *cfg;
	int rc;

	memset(m, 0, sizeof(*m));
	m->fd = -1;
	if (!c.maxkey || c.maxkey > 65535)
		c.maxkey = CFG_MAX_KEY;
	if (!c.maxval)
		c.maxval = CFG_MAX_VAL;
	if (!c.avg_item)
		c.avg_item = CFG_AVG_ITEM;
	snprintf(m->path, sizeof(m->path), "%s", c.path);

	if ((m->fd = open(c.path, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) < 0) {
		warn("%s: open:", c.path);
		return -1;
	}
	if (grab(m->fd) < 0) {
		warn("%s: still held by another kache", c.path);
		close(m->fd);
		m->fd = -1;
		return -1;
	}
	if (fstat(m->fd, &st) < 0) {
		warn("%s: stat:", c.path);
		goto fail;
	}

	if ((c.flags & KM_FRESH) || st.st_size == 0)
		rc = create(m, &c);
	else
		rc = adopt(m, &c, (u64)st.st_size);
	if (rc < 0)
		goto fail;

	m->hdr->flags &= ~(u32)ST_CLEAN;
	m->hdr->csum = csum(m->hdr);
	msync(m->base, ST_HDR_SZ, MS_SYNC);

	if (c.flags & KM_HUGE)
		madvise(m->base, m->size, MADV_HUGEPAGE);
	if (c.flags & KM_PREFAULT)
		madvise(m->base, m->size, MADV_WILLNEED);
	if (c.flags & KM_LOCKED) {
		if (mlock(m->base, m->size) < 0)
			warn("mlock: the store may still be paged out:");
		else
			info("locked %llu MiB into memory",
			     (unsigned long long)(m->size >> 20));
	}
	return 0;
fail:
	if (m->base)
		munmap(m->base, m->size);
	if (m->fd >= 0)
		close(m->fd);
	m->base = NULL;
	m->fd = -1;
	return -1;
}

int
map_sync(Map *m, int wait)
{
	if (!m->base)
		return 0;
	return msync(m->base, m->size, wait ? MS_SYNC : MS_ASYNC);
}

void
map_close(Map *m)
{
	if (!m->base)
		return;
	m->hdr->flags |= ST_CLEAN;
	m->hdr->csum = csum(m->hdr);
	map_sync(m, 1);
	munmap(m->base, m->size);
	close(m->fd);
	m->base = NULL;
	m->fd = -1;
}
