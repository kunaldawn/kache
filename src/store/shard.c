/* kache - the shard index, the eviction policy that keeps it fed, and
 * the sweeper that takes dead containers apart. */
#include <string.h>

#include "config.h"
#include "store/alloc.h"
#include "store/kkv.h"
#include "store/queue.h"
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
	s->dead = 0;
	s->dead_n = 0;
	s->pin[0] = s->pin[1] = 0;
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

/* ---- the graveyard ---------------------------------------------------
 *
 * Deleting a map of a hundred thousand fields, or a queue of a million
 * messages, would otherwise be one caller's problem: a single request
 * holding the shard lock while it walks a structure the size of the
 * arena.  Instead the container leaves the index at once and is taken
 * apart a few blocks at a time by the requests that follow it, which is
 * how a delete stays a constant time operation whatever it deletes.
 *
 * A dead container carries BLK_SUB, the flag that means "not in the
 * index".  That is exactly true of it now, and it keeps eviction and the
 * forward coalescing walk from ever looking at it again. */

static int
cont_reap(Map *m, Shard *s, Rec *r, u32 *budget)
{
	if (r->type == RT_KKV)
		return kkv_reap(m, s, r, budget);
	if (r->type == RT_QUEUE)
		return q_reap(m, s, r, budget);
	return 1;
}

u32
shd_sweep(Map *m, Shard *s, u32 budget)
{
	u32 spent = 0;

	while (s->dead && budget) {
		u64 payoff = s->dead;
		Rec *r = rec_at(m, payoff);
		Cont *c = (Cont *)cont_ctl(r);
		u32 before = budget;
		int done = cont_reap(m, s, r, &budget);

		spent += before - budget;
		if (!done)
			break;
		s->dead = c->gnext ? sub_off(s, c->gnext) : 0;
		s->dead_n--;
		alc_free(m, s, payoff, NULL, NULL);
		if (budget) {
			budget--;
			spent++;
		}
	}
	return spent;
}

void
shd_drain(Map *m, Shard *s)
{
	while (s->dead)
		shd_sweep(m, s, 4096);
}

static void
drop_slot(Map *m, Shard *s, u32 slot, u64 *fo, u32 *fs)
{
	u64 payoff = shd_buckets(m, s)[slot].off;
	Rec *r = rec_at(m, payoff);

	unlink_slot(m, s, slot);
	s->count--;
	if (r->type != RT_KV) {
		Cont *c = (Cont *)cont_ctl(r);

		c->gnext = s->dead ? sub_ref(s, s->dead) : 0;
		c->gpos = 0;
		s->dead = payoff;
		s->dead_n++;
		alc_mark(m, payoff, BLK_SUB);
		if (fo)
			*fo = 0;
		if (fs)
			*fs = 0;
		return;
	}
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
		if (!bk[i].off || shd_pinned(s, bk[i].off))
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
			if (bk[i].off && !shd_pinned(s, bk[i].off)) {
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
	if (!fs) {
		/* The victim was a container, so nothing came back yet.
		 * Reclaim some of it here instead: progress is progress,
		 * and the caller retries the allocation either way. */
		shd_sweep(m, s, CFG_SWEEP_BUDGET * 4);
		return 1;
	}
	while (fs < want) {
		u64 nx = fo + fs;
		Blk *nb;

		if (nx >= end)
			break;
		nb = alc_blk(m, nx);
		if (!BLK_USED(nb))
			break;              /* would have coalesced */
		/* A block that is not in the index cannot be evicted by
		 * probing for it: it is a container's, or a container that
		 * is already dead and waiting for the sweeper. */
		if (BLK_IS(nb, BLK_SUB) || shd_pinned(s, nx + ST_BLKHDR))
			break;
		s->evictions++;
		drop_slot(m, s, slot_of(m, s, nx + ST_BLKHDR), &fo, &fs);
		if (!fs)
			break;              /* a container, see above */
	}
	return 1;
}

static u64
alloc_flags(Map *m, Shard *s, u32 need, u32 flags)
{
	u32 want = (u32)ALIGNUP((u64)need + ST_BLKHDR, ST_GRAIN);
	u64 off;
	u32 i;

	if (want < ST_MINBLK)
		want = ST_MINBLK;
	if ((off = alc_alloc(m, s, need)) != 0)
		goto done;
	/* Memory the sweeper has not got to yet is still memory.  Ask it
	 * for some before evicting anything a client can still read. */
	if (s->dead) {
		shd_sweep(m, s, CFG_SWEEP_BUDGET);
		if ((off = alc_alloc(m, s, need)) != 0)
			goto done;
		shd_drain(m, s);
		if ((off = alc_alloc(m, s, need)) != 0)
			goto done;
	}
	for (i = 0; i < CFG_EVICT_ROUNDS; i++) {
		if (!evict_for(m, s, want))
			break;
		if ((off = alc_alloc(m, s, need)) != 0)
			goto done;
	}
	return 0;
done:
	if (flags)
		alc_mark(m, off, flags);
	return off;
}

u64
shd_alloc(Map *m, Shard *s, u32 need)
{
	return alloc_flags(m, s, need, 0);
}

u64
shd_alloc_sub(Map *m, Shard *s, u32 need)
{
	return alloc_flags(m, s, need, BLK_SUB);
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
 * and safer than trusting the metadata we crashed in the middle of.
 *
 * Containers make that walk two sided.  A block flagged BLK_SUB says it
 * belongs to one, but not that the container still wants it - the
 * pointer that did could have been half written.  So the walk runs three
 * times: once to make the block chain consistent again, once from every
 * surviving container to mark what it can still prove it owns, and once
 * more to give back everything nothing claimed. */

static int
rec_sane(const Map *m, const Rec *r, u32 blksize, u64 now)
{
	u32 need;

	if (r->klen == 0 || r->klen > m->maxkey || r->vlen > m->maxval)
		return 0;
	if (r->type == RT_KKV) {
		if (r->vlen != sizeof(KkvHdr))
			return 0;
	} else if (r->type == RT_QUEUE) {
		if (r->vlen != sizeof(QHdr))
			return 0;
	} else if (r->type != RT_KV) {
		return 0;
	}
	need = r->type == RT_KV ? REC_NEED(r->klen, r->vlen)
	                        : CONT_NEED(r->klen, r->vlen);
	if (need + ST_BLKHDR > blksize)
		return 0;
	if (r->hash != hash_bytes(r->data, r->klen, m->seed))
		return 0;
	if (rec_expired(r, now))
		return 0;
	return 1;
}

u64
shd_subok(const Map *m, const Shard *s, Ref ref, u32 need)
{
	u64 off, end = s->arena_off + s->arena_size - ST_GRAIN;
	Blk *b;

	if (!ref)
		return 0;
	off = s->arena_off + ((u64)(ref - 1) * ST_GRAIN);
	if (off < s->arena_off || off + ST_MINBLK > end)
		return 0;
	b = alc_blk(m, off);
	if (!BLK_USED(b) || !BLK_IS(b, BLK_SUB) || BLK_IS(b, BLK_MARK))
		return 0;
	if (BLK_SIZE(b) < (u64)need + ST_BLKHDR || off + BLK_SIZE(b) > end)
		return 0;
	return off + ST_BLKHDR;
}

/* Lay the arena out again from a predicate: a block the caller wants
 * kept stays where it is, everything else melts into the free runs
 * between.  Both the first and the last recovery pass are this walk. */
static int
relayout(Map *m, Shard *s, u64 now, int final, u64 limit)
{
	u64 end = s->arena_off + s->arena_size - ST_GRAIN;
	u64 o, run = 0;
	u32 prev = 0, runsz = 0;

	alc_reset_bins(s);
	for (o = s->arena_off; o < end; ) {
		Blk *b = alc_blk(m, o);
		u32 sz = BLK_SIZE(b);
		Rec *r = rec_at(m, o + ST_BLKHDR);
		int sub = BLK_IS(b, BLK_SUB), keep = 0;

		if (sz < ST_MINBLK || (sz & (ST_GRAIN - 1)) || o + sz > end)
			return -1;
		if (BLK_USED(b)) {
			if (final)
				keep = BLK_IS(b, BLK_MARK);
			else
				keep = sub || (s->count < limit &&
				               rec_sane(m, r, sz, now));
		}
		if (keep) {
			if (runsz) {
				alc_place_free(m, s, run, runsz, prev);
				prev = runsz;
				runsz = 0;
			}
			alc_place_used(m, s, o, sz, prev, sub ? BLK_SUB : 0);
			if (!final && !sub) {
				shd_link(m, s, r->hash, o + ST_BLKHDR);
				if (r->version > s->verseq)
					s->verseq = r->version;
			}
			prev = sz;
		} else {
			if (!runsz)
				run = o;
			runsz += sz;
		}
		o += sz;
	}
	if (o != end)
		return -1;
	if (runsz) {
		alc_place_free(m, s, run, runsz, prev);
		prev = runsz;
	}
	alc_place_end(m, s, prev);
	return 0;
}

/* Walk every key that survived the first pass and let the containers
 * prove what they own.  Claiming is part of proving: a block already
 * marked belongs to someone else, so a stale pointer cannot make two
 * containers share one field. */
static void
claim(Map *m, Shard *s, u64 now)
{
	Bucket *bk = shd_buckets(m, s);
	u32 i = 0;

	while (i < s->nbuckets) {
		Rec *r;
		int ok;

		if (!bk[i].off || BLK_IS(alc_of(m, bk[i].off), BLK_MARK)) {
			i++;
			continue;
		}
		r = rec_at(m, bk[i].off);
		if (r->type == RT_KKV)
			ok = kkv_check(m, s, r, now) == 0;
		else if (r->type == RT_QUEUE)
			ok = q_check(m, s, r, now) == 0;
		else
			ok = 1;
		if (ok) {
			alc_mark(m, bk[i].off, BLK_MARK);
			i++;
		} else {
			/* A backward shift can pull a later entry into this
			 * slot, so do not step past it. */
			unlink_slot(m, s, i);
			s->count--;
		}
	}
}

int
shd_rebuild(Map *m, Shard *s, u64 now)
{
	u64 limit = ((u64)s->nbuckets * CFG_LOAD_LIMIT) >> 8;

	if (s->arena_size < ST_GRAIN + ST_MINBLK || !s->nbuckets)
		return -1;

	memset(shd_buckets(m, s), 0, (size_t)s->nbuckets * sizeof(Bucket));
	s->count = 0;
	s->dead = 0;
	s->dead_n = 0;
	s->pin[0] = s->pin[1] = 0;
	if (!s->rng)
		s->rng = m->seed | 1ull;

	if (relayout(m, s, now, 0, limit) < 0)
		goto corrupt;
	claim(m, s, now);
	if (relayout(m, s, now, 1, limit) < 0)
		goto corrupt;
	s->inserts = s->count;
	return 0;

corrupt:
	shd_format(m, s, m->seed ^ s->arena_off);
	return -1;
}
