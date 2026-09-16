/* kache - on disk layout
 *
 * The whole store lives in one file mapped MAP_SHARED.  Nothing inside
 * the mapping is a pointer: every reference is a byte offset from the
 * start of the file, so the mapping can come back at a different address
 * after a restart.  Offset 0 is inside the header and therefore doubles
 * as the null reference.
 *
 *   +--------------------------------------------------+
 *   | Hdr                                     512 B    |
 *   | Shard[nshards]                          640 B ea |
 *   | shard 0: Bucket[nbuckets] | arena                |
 *   | shard 1: Bucket[nbuckets] | arena                |
 *   | ...                                              |
 *   +--------------------------------------------------+
 *
 * A shard owns its index and its arena outright, so one lock covers a
 * lookup, an allocation and an eviction with no further synchronisation.
 */
#ifndef KACHE_STORE_H
#define KACHE_STORE_H

#include "util/lock.h"
#include "util/util.h"

#define ST_MAGIC   0x4b41434845765fULL   /* "KACHEv_" */
#define ST_VERSION 1u

#define ST_HDR_SZ   512u
#define ST_SHARD_SZ 640u
#define ST_NBINS    64u

/* allocator granularity; record payloads end up 8 byte aligned */
#define ST_GRAIN    16u
#define ST_BLKHDR   8u
#define ST_MINBLK   32u

enum {
	ST_CLEAN = 1u << 0    /* unmounted without losing the mapping */
};

typedef struct Hdr {
	u64 magic;
	u32 version;
	u32 hdrsz;
	u64 filesz;
	u64 seed;             /* hash seed, persisted so keys keep landing
	                       * in the same shard across restarts */
	u32 nshards;
	u32 shard_shift;      /* nshards == 1u << shard_shift */
	u64 shards_off;
	u64 slice;            /* bytes owned by one shard */
	u32 maxkey;
	u32 maxval;
	u32 shardsz;
	u32 flags;
	u64 created;          /* ms since the epoch */
	u64 opened;
	u64 csum;             /* over the header with csum zeroed */
} Hdr;

/* One index slot.  Keeping the full hash next to the offset means a
 * probe rejects a collision without touching the record at all. */
typedef struct Bucket {
	u64 hash;
	u64 off;              /* record offset, 0 when the slot is empty */
} Bucket;

/* Block header of the arena allocator.  Boundary tags in both
 * directions: size describes this block, prev the one before it, so a
 * free coalesces with either neighbour in constant time.  Sizes are
 * multiples of ST_GRAIN, which leaves the low bits for flags. */
typedef struct Blk {
	u32 prev;             /* size of the preceding block, 0 if first */
	u32 size;             /* total size including this header | flags */
} Blk;

#define BLK_INUSE   1u
#define BLK_SIZE(b) ((b)->size & ~(ST_GRAIN - 1u))
#define BLK_USED(b) ((b)->size & BLK_INUSE)

/* A record, stored in a block's payload, immediately followed by the key
 * bytes and then the value bytes. */
typedef struct Rec {
	u64 hash;
	u64 expire;           /* absolute ms, 0 = never expires */
	u64 version;          /* CAS token, bumped on every mutation */
	u32 atime;            /* wall clock seconds, for sampled LRU */
	u32 vlen;
	u32 flags;            /* opaque, echoed back to the client */
	u16 klen;
	u16 pad;
	u8  data[];
} Rec;

#define REC_HDR    ((u32)sizeof(Rec))
#define REC_NEED(kl, vl) ((u32)(REC_HDR + (kl) + (vl)))

static inline u8 *rec_key(Rec *r) { return r->data; }
static inline u8 *rec_val(Rec *r) { return r->data + r->klen; }

typedef struct Shard {
	Lock lock;
	u32  nbuckets;        /* power of two */
	u64  buckets_off;
	u64  arena_off;
	u64  arena_size;
	u64  count;           /* live records */
	u64  used;            /* bytes handed out by the allocator */
	u64  verseq;          /* source of CAS tokens */
	u64  rng;             /* eviction sampling state */
	u64  inserts;
	u64  evictions;
	u64  expirations;
	u64  bins[ST_NBINS];  /* free list heads, 0 = empty */
	u8   pad[ST_SHARD_SZ - 88 - ST_NBINS * 8];
} Shard;

_Static_assert(sizeof(Rec) == 40, "record header must stay 40 bytes");
_Static_assert(sizeof(Blk) == 8, "block header must stay 8 bytes");
_Static_assert(sizeof(Bucket) == 16, "bucket must stay 16 bytes");
_Static_assert(sizeof(Shard) == ST_SHARD_SZ, "shard size drifted");
_Static_assert(sizeof(Hdr) <= ST_HDR_SZ, "header does not fit");

#endif /* KACHE_STORE_H */
