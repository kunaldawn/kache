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
 *
 * A key holds one of three things, named by Rec.type: a value (KV), a
 * nested key/value map (KKV) or a queue.  The two container types keep a
 * fixed size control block where a plain value would sit, and everything
 * hanging off it - slot tables, field records, queue segments - is an
 * ordinary arena block flagged BLK_SUB.  Sub blocks are not in the index:
 * they are reachable only from their container, which is why they carry a
 * back reference to it and why eviction never picks one directly. */
#ifndef KACHE_STORE_H
#define KACHE_STORE_H

#include "util/lock.h"
#include "util/util.h"

#define ST_MAGIC   0x4b41434845765fULL   /* "KACHEv_" */
#define ST_VERSION 2u

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
#define BLK_SUB     2u    /* owned by a container, never indexed */
#define BLK_MARK    4u    /* reachable; set and cleared by recovery only */
#define BLK_FLAGS   (ST_GRAIN - 1u)
#define BLK_SIZE(b) ((b)->size & ~BLK_FLAGS)
#define BLK_USED(b) ((b)->size & BLK_INUSE)
#define BLK_IS(b,f) (((b)->size & (f)) != 0)

/* What a key holds.  Zero is a plain value, so a record written before
 * containers existed reads back as one. */
enum {
	RT_KV    = 0,
	RT_KKV   = 1,
	RT_QUEUE = 2
};

/* A record, stored in a block's payload, immediately followed by the key
 * bytes and then the value bytes.  For a container the value bytes are
 * its control block (KkvHdr or QHdr) rather than caller data. */
typedef struct Rec {
	u64 hash;
	u64 expire;           /* absolute ms, 0 = never expires */
	u64 version;          /* CAS token, bumped on every mutation */
	u32 atime;            /* wall clock seconds, for sampled LRU */
	u32 vlen;
	u32 flags;            /* opaque, echoed back to the client */
	u16 klen;
	u8  type;             /* RT_* */
	u8  pad;
	u8  data[];
} Rec;

#define REC_HDR    ((u32)sizeof(Rec))
#define REC_NEED(kl, vl) ((u32)(REC_HDR + (kl) + (vl)))

static inline u8 *rec_key(Rec *r) { return r->data; }
static inline u8 *rec_val(Rec *r) { return r->data + r->klen; }

/* A plain value is bytes and can start anywhere.  A container's control
 * block is a row of 64 bit counters, so it has to be 8 byte aligned, and
 * the key in front of it is padded up to the next multiple of eight to
 * make that true.  The key still comes first, because that is what the
 * index compares against without knowing what kind of key it is. */
#define CONT_OFF(kl)      ((u32)ALIGNUP((u32)(kl), 8u))
#define CONT_NEED(kl, vl) ((u32)(REC_HDR + CONT_OFF(kl) + (vl)))

static inline void *cont_ctl(Rec *r) { return r->data + CONT_OFF(r->klen); }

/* ---- containers ------------------------------------------------------
 *
 * Inside a container every reference is a sub block ref: the block's
 * offset measured from the start of its shard's arena, in grains, biased
 * by one so that 0 stays the null reference.  An arena is at most 4 GiB
 * and blocks are 16 byte aligned, so 32 bits is more than enough - which
 * halves the size of a slot table and of a queue segment header. */
typedef u32 Ref;

/* The head of every container control block.  The sweeper only needs
 * these two fields, so it can walk a graveyard of mixed types. */
typedef struct Cont {
	Ref gnext;            /* next dead container, 0 at the end */
	u32 gpos;             /* how far the sweeper got through this one */
} Cont;

/* A nested map.  tab points at a KkvTab of nslots open addressed slots;
 * an empty map has no table at all. */
typedef struct KkvHdr {
	Ref gnext;
	u32 gpos;
	Ref tab;
	u32 nslots;           /* power of two, 0 when tab is 0 */
	u32 count;            /* live slots, expired ones included */
	u32 pad;
	u64 fseq;             /* source of per field CAS tokens */
	u64 bytes;            /* field payload bytes, for accounting */
} KkvHdr;

typedef struct KkvSlot {
	u32 tag;              /* low 32 bits of the field hash */
	Ref ref;              /* field block, 0 when the slot is empty */
} KkvSlot;

typedef struct KkvTab {
	Ref owner;            /* the record this table belongs to */
	u32 nslots;
	KkvSlot slot[];
} KkvTab;

/* One field.  Shaped like a Rec and validated the same way, but it lives
 * outside the index, so it carries a back reference instead of being
 * found by probing. */
typedef struct KkvFld {
	u64 hash;
	u64 expire;           /* absolute ms, 0 = lives as long as the map */
	u64 version;
	Ref owner;
	u32 vlen;
	u32 flags;
	u16 klen;
	u16 pad;
	u8  data[];           /* field key, then value */
} KkvFld;

#define FLD_HDR    ((u32)sizeof(KkvFld))
#define FLD_NEED(kl, vl) ((u32)(FLD_HDR + (kl) + (vl)))

static inline u8 *fld_key(KkvFld *f) { return f->data; }
static inline u8 *fld_val(KkvFld *f) { return f->data + f->klen; }

/* A deque.  Entries live inside segments rather than in blocks of their
 * own: one allocation feeds dozens of pushes, and the bytes of a queue
 * that is being drained stay contiguous. */
typedef struct QHdr {
	Ref gnext;
	u32 gpos;
	Ref head;             /* leftmost segment, 0 when the queue is empty */
	Ref tail;
	u32 segsz;            /* size of the next segment to allocate */
	u32 pad;
	u64 count;
	u64 bytes;            /* entry payload bytes */
	u64 seq;              /* source of entry ids, and the queue's CAS token */
} QHdr;

/* Live bytes of a segment are data[head .. tail).  Both ends move: a pop
 * from the left advances head, a push on the left fills backwards into
 * the room that leaves. */
typedef struct QSeg {
	Ref next;             /* toward the tail */
	Ref prev;
	Ref owner;
	u32 cap;              /* usable bytes in data[] */
	u32 head;
	u32 tail;
	u32 count;
	u32 pad;
	u8  data[];
} QSeg;

/* One entry, with its frame length at both ends so that a pop from
 * either side is a constant time step - the same boundary tag trick the
 * allocator uses one level down. */
typedef struct QEnt {
	u32 len;              /* whole frame, a multiple of 8 */
	u32 vlen;
	u64 expire;           /* absolute ms, 0 = lives as long as the queue */
	u64 id;
	u32 flags;
	u32 pad;
	u8  data[];
} QEnt;

#define QENT_HDR   ((u32)sizeof(QEnt))
/* header, payload, and the trailing copy of the length, rounded to 8 */
#define QENT_NEED(vl) ((u32)ALIGNUP((u64)QENT_HDR + (vl) + 4u, 8u))

static inline u32 *qent_back(QEnt *e) { return (u32 *)((u8 *)e + e->len - 4); }

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
	u64  dead;            /* graveyard head: payload offset, 0 when empty */
	u64  dead_n;          /* containers waiting to be reclaimed */
	u64  pin[2];          /* records eviction must not take right now;
	                       * two, because moving an entry between two
	                       * queues of the same shard holds both */
	u64  bins[ST_NBINS];  /* free list heads, 0 = empty */
	u8   pad[ST_SHARD_SZ - 120 - ST_NBINS * 8];
} Shard;

_Static_assert(sizeof(Rec) == 40, "record header must stay 40 bytes");
_Static_assert(sizeof(KkvFld) == 40, "field header must stay 40 bytes");
_Static_assert(sizeof(KkvHdr) == 40, "map control block drifted");
_Static_assert(sizeof(KkvTab) == 8, "slot table header drifted");
_Static_assert(sizeof(KkvSlot) == 8, "slot must stay 8 bytes");
_Static_assert(sizeof(QHdr) == 48, "queue control block drifted");
_Static_assert(sizeof(QSeg) == 32, "segment header drifted");
_Static_assert(sizeof(QEnt) == 32, "entry header drifted");
_Static_assert(sizeof(Blk) == 8, "block header must stay 8 bytes");
_Static_assert(sizeof(Bucket) == 16, "bucket must stay 16 bytes");
_Static_assert(sizeof(Shard) == ST_SHARD_SZ, "shard size drifted");
_Static_assert(sizeof(Hdr) <= ST_HDR_SZ, "header does not fit");
_Static_assert(sizeof(Cont) == 8, "container prefix drifted");

/* both containers must start with the fields the sweeper reads */
_Static_assert(offsetof(KkvHdr, gnext) == offsetof(Cont, gnext) &&
               offsetof(KkvHdr, gpos) == offsetof(Cont, gpos) &&
               offsetof(QHdr, gnext) == offsetof(Cont, gnext) &&
               offsetof(QHdr, gpos) == offsetof(Cont, gpos),
               "container prefix must be common to every container type");

/* ---- sub block references -------------------------------------------- */

static inline Ref
sub_ref(const Shard *s, u64 payoff)
{
	return (Ref)(((payoff - ST_BLKHDR - s->arena_off) / ST_GRAIN) + 1);
}

static inline u64
sub_off(const Shard *s, Ref ref)
{
	return s->arena_off + ((u64)(ref - 1) * ST_GRAIN) + ST_BLKHDR;
}

#endif /* KACHE_STORE_H */
