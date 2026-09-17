/* kache - the arena allocator.  Segregated fit with boundary tags; see
 * doc/DESIGN.md for why it coalesces instead of using size classed slabs. */
#include <string.h>

#include "config.h"
#include "store/alloc.h"

/* Free blocks are threaded through their own payload. */
typedef struct Node {
	u64 next;
	u64 prev;
} Node;

static inline Node *
node(const Map *m, u64 blkoff)
{
	return (Node *)(m->base + blkoff + ST_BLKHDR);
}

/* Bins 0..15 are exact multiples of 32 bytes, the sizes that dominate a
 * cache.  Above that, four bins per power of two. */
static inline u32
bin_of(u32 sz)
{
	u32 k, b;

	if (sz < 512)
		return sz >> 5;
	k = 31u - (u32)__builtin_clz(sz);
	if (k > 20)
		k = 20;
	b = 16u + ((k - 9u) << 2) + ((sz >> (k - 2u)) & 3u);
	return b < ST_NBINS ? b : ST_NBINS - 1;
}

static void
bin_push(Map *m, Shard *s, u64 blkoff, u32 sz)
{
	u32 b = bin_of(sz);
	u64 head = s->bins[b];
	Node *n = node(m, blkoff);

	n->prev = 0;
	n->next = head;
	if (head)
		node(m, head)->prev = blkoff;
	s->bins[b] = blkoff;
}

static void
bin_pop(Map *m, Shard *s, u64 blkoff, u32 sz)
{
	Node *n = node(m, blkoff);

	if (n->prev)
		node(m, n->prev)->next = n->next;
	else
		s->bins[bin_of(sz)] = n->next;
	if (n->next)
		node(m, n->next)->prev = n->prev;
}

u64
alc_max(const Shard *s)
{
	if (s->arena_size < ST_GRAIN * 2 + ST_MINBLK)
		return 0;
	return s->arena_size - ST_GRAIN - ST_BLKHDR;
}

void
alc_init(Map *m, Shard *s)
{
	u64 end = s->arena_off + s->arena_size;
	u32 first = (u32)(s->arena_size - ST_GRAIN);
	Blk *b, *sentinel;

	memset(s->bins, 0, sizeof(s->bins));
	s->used = 0;
	if (s->arena_size < ST_GRAIN + ST_MINBLK)
		return;

	b = alc_blk(m, s->arena_off);
	b->prev = 0;
	b->size = first;                     /* free */

	/* The last grain is a permanently in use block so that forward
	 * coalescing and forward walks always terminate. */
	sentinel = alc_blk(m, end - ST_GRAIN);
	sentinel->prev = first;
	sentinel->size = ST_GRAIN | BLK_INUSE;

	bin_push(m, s, s->arena_off, first);
}

u32
alc_cap(const Map *m, u64 payoff)
{
	return BLK_SIZE(alc_blk(m, payoff - ST_BLKHDR)) - ST_BLKHDR;
}

u64
alc_alloc(Map *m, Shard *s, u32 need)
{
	u32 want = (u32)ALIGNUP((u64)need + ST_BLKHDR, ST_GRAIN);
	u32 b, sz, rest;
	u64 off = 0, cur;
	int scan;

	if (want < ST_MINBLK)
		want = ST_MINBLK;
	if (want > s->arena_size)
		return 0;

	for (b = bin_of(want); b < ST_NBINS; b++) {
		scan = 0;
		for (cur = s->bins[b]; cur; cur = node(m, cur)->next) {
			if (BLK_SIZE(alc_blk(m, cur)) >= want) {
				off = cur;
				goto found;
			}
			/* Only the topmost bins mix sizes enough for this
			 * walk to matter; bound it so a pathological free
			 * list cannot slow an allocation down. */
			if (++scan >= 128)
				break;
		}
	}
	return 0;
found:
	sz = BLK_SIZE(alc_blk(m, off));
	bin_pop(m, s, off, sz);
	rest = sz - want;
	if (rest >= ST_MINBLK) {
		u64 ro = off + want;
		Blk *rb = alc_blk(m, ro);

		alc_blk(m, off)->size = want | BLK_INUSE;
		rb->prev = want;
		rb->size = rest;
		alc_blk(m, ro + rest)->prev = rest;
		bin_push(m, s, ro, rest);
		s->used += want;
	} else {
		alc_blk(m, off)->size = sz | BLK_INUSE;
		s->used += sz;
	}
	return off + ST_BLKHDR;
}

void
alc_free(Map *m, Shard *s, u64 payoff, u64 *blkoff, u32 *blksize)
{
	u64 off = payoff - ST_BLKHDR;
	Blk *b = alc_blk(m, off);
	u32 sz = BLK_SIZE(b);
	u64 next;

	s->used -= sz;

	next = off + sz;
	if (!BLK_USED(alc_blk(m, next))) {
		u32 nsz = BLK_SIZE(alc_blk(m, next));
		bin_pop(m, s, next, nsz);
		sz += nsz;
	}
	if (b->prev) {
		u64 prev = off - b->prev;
		Blk *pb = alc_blk(m, prev);

		if (!BLK_USED(pb)) {
			bin_pop(m, s, prev, BLK_SIZE(pb));
			sz += BLK_SIZE(pb);
			off = prev;
			b = pb;
		}
	}
	b->size = sz;                        /* clears BLK_INUSE */
	alc_blk(m, off + sz)->prev = sz;
	bin_push(m, s, off, sz);

	if (blkoff)
		*blkoff = off;
	if (blksize)
		*blksize = sz;
}

void
alc_reset_bins(Shard *s)
{
	memset(s->bins, 0, sizeof(s->bins));
	s->used = 0;
}

void
alc_place_free(Map *m, Shard *s, u64 off, u32 size, u32 prev)
{
	Blk *b = alc_blk(m, off);

	b->prev = prev;
	b->size = size;
	bin_push(m, s, off, size);
}

void
alc_place_used(Map *m, Shard *s, u64 off, u32 size, u32 prev, u32 flags)
{
	Blk *b = alc_blk(m, off);

	b->prev = prev;
	b->size = size | BLK_INUSE | flags;
	s->used += size;
}

void
alc_place_end(Map *m, Shard *s, u32 prev)
{
	Blk *b = alc_blk(m, s->arena_off + s->arena_size - ST_GRAIN);

	b->prev = prev;
	b->size = ST_GRAIN | BLK_INUSE;
}
