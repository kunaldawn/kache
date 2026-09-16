/* kache - the shard index and the eviction policy that keeps it fed. */
#include <string.h>

#include "config.h"
#include "store/alloc.h"
#include "store/shard.h"
#include "util/clk.h"
#include "util/hash.h"

static inline Rec *
rec_at(const Map *m, u64 payoff)
{
	return (Rec *)(m->base + payoff);
}

void
shd_format(Map *m, Shard *s, u64 seed)
{
	memset(shd_buckets(m, s), 0, (size_t)s->nbuckets * sizeof(Bucket));
	s->count = 0;
	s->inserts = 0;
	s->evictions = 0;
	s->expirations = 0;
	s->verseq = 0;
	s->rng = seed | 1ull;
	alc_init(m, s);
}

void
shd_clear(Map *m, Shard *s)
{
	u64 seed = s->rng;

	shd_format(m, s, seed);
}

Rec *
shd_lookup(const Map *m, const Shard *s, u64 h, const void *k, u32 kl,
           u32 *slot)
{
	Bucket *bk = shd_buckets(m, s);
	u32 mask = s->nbuckets - 1;
	u32 i = (u32)h & mask;

	for (;;) {
		u64 off = bk[i].off;

		if (!off)
			return NULL;
		if (bk[i].hash == h) {
			Rec *r = rec_at(m, off);
			if (r->klen == kl && memcmp(r->data, k, kl) == 0) {
				*slot = i;
				return r;
			}
		}
		i = (i + 1) & mask;
	}
}

/* Locate the slot holding a known record.  Used when eviction picks a
 * victim by arena position rather than by key. */
static u32
slot_of(const Map *m, const Shard *s, u64 payoff)
{
	Bucket *bk = shd_buckets(m, s);
	u32 mask = s->nbuckets - 1;
	u64 h = rec_at(m, payoff)->hash;
	u32 i = (u32)h & mask;

	while (bk[i].off != payoff)
		i = (i + 1) & mask;
	return i;
}

void
shd_link(Map *m, Shard *s, u64 h, u64 payoff)
{
	Bucket *bk = shd_buckets(m, s);
	u32 mask = s->nbuckets - 1;
	u32 i = (u32)h & mask;

	while (bk[i].off)
		i = (i + 1) & mask;
	bk[i].hash = h;
	bk[i].off = payoff;
	s->count++;
	s->inserts++;
}

/* Backward shift deletion: pull following entries of the same probe run
 * back into the hole so lookups still terminate on an empty slot. */
static void
unlink_slot(const Map *m, const Shard *s, u32 i)
{
	Bucket *bk = shd_buckets(m, s);
	u32 mask = s->nbuckets - 1;
	u32 j = i, k;

	bk[i].hash = 0;
	bk[i].off = 0;
	for (;;) {
		j = (j + 1) & mask;
		if (!bk[j].off)
			return;
		k = (u32)bk[j].hash & mask;
		/* entries whose home is cyclically within (i, j] are
		 * already reachable and must not move */
		if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
			continue;
		bk[i] = bk[j];
		bk[j].hash = 0;
		bk[j].off = 0;
		i = j;
	}
}

static void
drop_slot(Map *m, Shard *s, u32 slot, u64 *fo, u32 *fs)
{
	u64 payoff = shd_buckets(m, s)[slot].off;

	unlink_slot(m, s, slot);
	s->count--;
	alc_free(m, s, payoff, fo, fs);
}

void
shd_drop(Map *m, Shard *s, u32 slot)
{
	drop_slot(m, s, slot, NULL, NULL);
}

/* Pick an eviction victim: expired entries go first, otherwise the
 * least recently used of a small random sample. */
static int
pick_victim(const Map *m, Shard *s, u32 *out)
{
	Bucket *bk = shd_buckets(m, s);
	u32 mask = s->nbuckets - 1;
	u64 now = now_ms();
	u32 best = 0, i, n;
	u32 best_atime = 0;
	int found = 0;

	if (!s->count)
		return 0;
	for (n = 0; n < CFG_EVICT_SAMPLES; n++) {
		Rec *r;

		i = (u32)rng_next(&s->rng) & mask;
		if (!bk[i].off)
			continue;
		r = rec_at(m, bk[i].off);
		if (rec_expired(r, now)) {
			s->expirations++;
			*out = i;
			return 1;
		}
		if (!found || r->atime < best_atime) {
			best_atime = r->atime;
			best = i;
			found = 1;
		}
	}
	if (!found) {
		/* Sparse table with a full arena: walk until we hit
		 * something rather than spin on the sampler. */
		i = (u32)rng_next(&s->rng) & mask;
		for (n = 0; n <= mask; n++, i = (i + 1) & mask) {
			if (bk[i].off) {
				best = i;
				found = 1;
				break;
			}
		}
	}
	if (!found)
		return 0;
	*out = best;
	s->evictions++;
	return 1;
}

/* Evict a victim and then keep evicting its forward neighbours until the
 * coalesced free run is large enough.  This is what lets a big value in
 * even when the arena is a mosaic of small live records. */
static int
evict_for(Map *m, Shard *s, u32 want)
{
	u64 end = s->arena_off + s->arena_size - ST_GRAIN;
	u64 fo;
	u32 fs, slot;

	if (!pick_victim(m, s, &slot))
		return 0;
	drop_slot(m, s, slot, &fo, &fs);
	while (fs < want) {
		u64 nx = fo + fs;
		Blk *nb;

		if (nx >= end)
			break;
		nb = alc_blk(m, nx);
		if (!BLK_USED(nb))
			break;              /* would have coalesced */
		s->evictions++;
		drop_slot(m, s, slot_of(m, s, nx + ST_BLKHDR), &fo, &fs);
	}
	return 1;
}

u64
shd_alloc(Map *m, Shard *s, u32 need)
{
	u32 want = (u32)ALIGNUP((u64)need + ST_BLKHDR, ST_GRAIN);
	u64 off;
	u32 i;

	if (want < ST_MINBLK)
		want = ST_MINBLK;
	if ((off = alc_alloc(m, s, need)) != 0)
		return off;
	for (i = 0; i < CFG_EVICT_ROUNDS; i++) {
		if (!evict_for(m, s, want))
			break;
		if ((off = alc_alloc(m, s, need)) != 0)
			return off;
	}
	return 0;
}

void
shd_reserve(Map *m, Shard *s)
{
	u64 limit = ((u64)s->nbuckets * CFG_LOAD_LIMIT) >> 8;
	u32 slot;

	while (s->count + 1 > limit) {
		if (!pick_victim(m, s, &slot))
			return;
		drop_slot(m, s, slot, NULL, NULL);
	}
}

/* ---- recovery -------------------------------------------------------
 *
 * After an unclean shutdown the index and the free lists are suspect,
 * but the arena itself is a self describing chain of blocks.  Walking it
 * and rebuilding everything from the records that survive is both faster
 * and safer than trusting the metadata we crashed in the middle of. */

static int
rec_sane(const Map *m, const Rec *r, u32 blksize, u64 now)
{
	u32 need;

	if (r->klen == 0 || r->klen > m->maxkey || r->vlen > m->maxval)
		return 0;
	need = REC_NEED(r->klen, r->vlen);
	if (need + ST_BLKHDR > blksize)
		return 0;
	if (r->hash != hash_bytes(r->data, r->klen, m->seed))
		return 0;
	if (rec_expired(r, now))
		return 0;
	return 1;
}

int
shd_rebuild(Map *m, Shard *s, u64 now)
{
	u64 end = s->arena_off + s->arena_size - ST_GRAIN;
	u64 limit = ((u64)s->nbuckets * CFG_LOAD_LIMIT) >> 8;
	u64 o = s->arena_off, run = 0;
	u32 prev = 0, runsz = 0;

	if (s->arena_size < ST_GRAIN + ST_MINBLK || !s->nbuckets)
		return -1;

	memset(shd_buckets(m, s), 0, (size_t)s->nbuckets * sizeof(Bucket));
	alc_reset_bins(s);
	s->count = 0;
	if (!s->rng)
		s->rng = m->seed | 1ull;

	while (o < end) {
		Blk *b = alc_blk(m, o);
		u32 sz = BLK_SIZE(b);
		Rec *r;

		if (sz < ST_MINBLK || (sz & (ST_GRAIN - 1)) || o + sz > end)
			goto corrupt;
		r = rec_at(m, o + ST_BLKHDR);
		if (BLK_USED(b) && s->count < limit && rec_sane(m, r, sz, now)) {
			if (runsz) {
				alc_place_free(m, s, run, runsz, prev);
				prev = runsz;
				runsz = 0;
			}
			alc_place_used(m, s, o, sz, prev);
			shd_link(m, s, r->hash, o + ST_BLKHDR);
			if (r->version > s->verseq)
				s->verseq = r->version;
			prev = sz;
		} else {
			if (!runsz)
				run = o;
			runsz += sz;
		}
		o += sz;
	}
	if (o != end)
		goto corrupt;
	if (runsz) {
		alc_place_free(m, s, run, runsz, prev);
		prev = runsz;
	}
	alc_place_end(m, s, prev);
	s->inserts = s->count;
	return 0;

corrupt:
	shd_format(m, s, m->seed ^ s->arena_off);
	return -1;
}
