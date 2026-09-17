/* kache - one shard: open addressed index, eviction, reclamation
 *
 * Linear probing with the full hash cached in the slot, and backward
 * shift deletion so the table never accumulates tombstones.  Every
 * function here expects the shard lock to be held. */
#ifndef KACHE_SHARD_H
#define KACHE_SHARD_H

#include "store/map.h"
#include "store/store.h"
#include "util/util.h"

static inline Bucket *
shd_buckets(const Map *m, const Shard *s)
{
	return (Bucket *)(m->base + s->buckets_off);
}

static inline int
rec_expired(const Rec *r, u64 now)
{
	return r->expire != 0 && r->expire <= now;
}

void shd_format(Map *m, Shard *s, u64 seed);

/* find a key; on miss returns NULL.  slot is only meaningful on a hit */
Rec *shd_lookup(const Map *m, const Shard *s, u64 h, const void *k, u32 kl,
                u32 *slot);
/* publish an already allocated record */
void shd_link(Map *m, Shard *s, u64 h, u64 payoff);
/* remove the record occupying a slot and return its storage; a container
 * goes to the graveyard instead, to be taken apart a piece at a time */
void shd_drop(Map *m, Shard *s, u32 slot);
/* allocate record storage, evicting as much as it takes */
u64  shd_alloc(Map *m, Shard *s, u32 need);
/* the same, for a block that hangs off a container */
u64  shd_alloc_sub(Map *m, Shard *s, u32 need);
/* make room in the index for one more key */
void shd_reserve(Map *m, Shard *s);
/* drop everything */
void shd_clear(Map *m, Shard *s);
/* rebuild index and free lists from the arena; 0 ok, -1 reformatted */
int  shd_rebuild(Map *m, Shard *s, u64 now);
/* Recovery: is ref a live, unclaimed sub block of at least need bytes?
 * Returns its payload offset, or 0.  Who owns it is the caller's to
 * check, since each container type stores that in a place of its own. */
u64  shd_subok(const Map *m, const Shard *s, Ref ref, u32 need);

/* Take apart at most budget sub blocks of the dead containers, returning
 * how many were reclaimed.  Called on the way into any operation that
 * may allocate, so a container of a million entries is dismantled by the
 * traffic that follows it instead of by the caller that killed it. */
u32  shd_sweep(Map *m, Shard *s, u32 budget);
/* reclaim the whole graveyard, however long that takes */
void shd_drain(Map *m, Shard *s);

/* While an operation walks a container, eviction must not take that
 * container out from under it.  Sub blocks are never eviction candidates
 * to begin with, so one pinned record covers the whole subtree.  There
 * are two slots because moving an entry from one queue to another holds
 * a source and a destination, and they may share a shard. */
static inline void
shd_pin(Shard *s, int i, u64 payoff)
{
	s->pin[i] = payoff;
}

static inline void
shd_unpin(Shard *s, int i)
{
	s->pin[i] = 0;
}

static inline int
shd_pinned(const Shard *s, u64 payoff)
{
	return payoff == s->pin[0] || payoff == s->pin[1];
}

#endif /* KACHE_SHARD_H */
