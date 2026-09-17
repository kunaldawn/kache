/* kache - the per shard arena allocator
 *
 * A segregated fit allocator with boundary tags, living entirely inside
 * the mapped file.  Because a shard owns its arena, every call here runs
 * under that shard's lock and needs no atomics.
 *
 * Coalescing is what keeps a cache healthy: evicting arbitrary records
 * of any size eventually produces a contiguous run large enough for the
 * next insert, so the store never calcifies around one item size the way
 * a fixed slab allocator does. */
#ifndef KACHE_ALLOC_H
#define KACHE_ALLOC_H

#include "store/map.h"
#include "store/store.h"
#include "util/util.h"

/* largest payload the arena can ever hand out */
u64  alc_max(const Shard *s);
/* lay out an empty arena: one free block plus the end sentinel */
void alc_init(Map *m, Shard *s);
/* payload offset, or 0 when nothing large enough is free */
u64  alc_alloc(Map *m, Shard *s, u32 need);
/* release a payload; reports the coalesced free block left behind */
void alc_free(Map *m, Shard *s, u64 payoff, u64 *blkoff, u32 *blksize);
/* usable bytes of an allocated payload */
u32  alc_cap(const Map *m, u64 payoff);

/* Reuse a block in place when it is a decent fit; a much larger block
 * would pin memory the rest of the shard could use. */
static inline int
alc_fits(u32 cap, u32 need)
{
	return cap >= need && (cap <= 128 || need >= cap / 2);
}

/* recovery helpers: rewrite the arena block by block */
void alc_reset_bins(Shard *s);
void alc_place_free(Map *m, Shard *s, u64 off, u32 size, u32 prev);
void alc_place_used(Map *m, Shard *s, u64 off, u32 size, u32 prev, u32 flags);
void alc_place_end(Map *m, Shard *s, u32 prev);

static inline Blk *
alc_blk(const Map *m, u64 blkoff)
{
	return (Blk *)(m->base + blkoff);
}

/* the block behind an allocated payload */
static inline Blk *
alc_of(const Map *m, u64 payoff)
{
	return alc_blk(m, payoff - ST_BLKHDR);
}

/* Flags ride in the low bits of the size field, which alignment leaves
 * free.  Freeing rewrites size outright and so clears them all. */
static inline void
alc_mark(const Map *m, u64 payoff, u32 flags)
{
	alc_of(m, payoff)->size |= flags;
}

static inline void
alc_unmark(const Map *m, u64 payoff, u32 flags)
{
	alc_of(m, payoff)->size &= ~flags;
}

#endif /* KACHE_ALLOC_H */
