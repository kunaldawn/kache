/* kache - one shard: open addressed index plus eviction
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
/* remove the record occupying a slot and return its storage */
void shd_drop(Map *m, Shard *s, u32 slot);
/* allocate record storage, evicting as much as it takes */
u64  shd_alloc(Map *m, Shard *s, u32 need);
/* make room in the index for one more key */
void shd_reserve(Map *m, Shard *s);
/* drop everything */
void shd_clear(Map *m, Shard *s);
/* rebuild index and free lists from the arena; 0 ok, -1 reformatted */
int  shd_rebuild(Map *m, Shard *s, u64 now);

#endif /* KACHE_SHARD_H */
