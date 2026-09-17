/* kache - the per worker hot set
 *
 * A key that carries a disproportionate share of the load is the one
 * case the sharded store is worst at.  Every request for it lands in
 * the same shard, takes the same exclusive lock and writes the same
 * cache line, so the cores queue behind each other and throughput falls
 * as cores are added.  Nothing about the lock can fix that: the
 * contention is the sharing, not the implementation.
 *
 * So this layer removes the sharing instead.  Each worker owns a small
 * set of finished responses, in memory no other thread reads or writes,
 * and a hot GET is answered by copying one out and patching its Date.
 * The store is never reached, no lock is taken, and no line is pulled
 * away from another core, so the answer costs the same on eight workers
 * as on one.
 *
 * Everything here is single threaded by construction.  There are no
 * atomics because there is no sharing to order: a Hot belongs to one
 * worker for the life of the process.
 *
 * The cost is staleness.  An entry stands for CFG_HOT_MS, so a write on
 * one worker is invisible to the others for that long.  A write does
 * clear the set of the worker that took it, and a connection belongs to
 * one worker for its whole life, so a client still reads its own
 * writes; what it cannot see at once is somebody else's. */
#ifndef KACHE_HOT_H
#define KACHE_HOT_H

#include "config.h"
#include "util/util.h"

_Static_assert((CFG_HOT_SLOTS & (CFG_HOT_SLOTS - 1)) == 0,
               "CFG_HOT_SLOTS must be a power of two");
_Static_assert((CFG_HOT_DOOR & (CFG_HOT_DOOR - 1)) == 0,
               "CFG_HOT_DOOR must be a power of two");
_Static_assert(CFG_HOT_ADMIT > 0 && CFG_HOT_ADMIT < 256,
               "CFG_HOT_ADMIT must fit in the doorkeeper's byte");
_Static_assert(CFG_HOT_KEY <= CFG_MAX_KEY,
               "CFG_HOT_KEY cannot exceed the largest key there is");

/* One finished response.  hash is the whole 64 bit key hash, so a
 * collision is rejected before the key comparison; the key is kept as
 * well because a 64 bit hash is not a proof and serving the wrong
 * value is not a trade worth making. */
typedef struct HotEnt {
	u64 hash;                 /* 0 when the slot is empty */
	u64 until;                /* ms; the entry is stale from here */
	u32 gen;                  /* the set's generation when filled */
	u32 klen;
	u32 rlen;                 /* bytes of the finished response */
	u32 doff;                 /* Date value's offset within resp */
	u8  key[CFG_HOT_KEY];
	u8  resp[CFG_HOT_RESP];
} HotEnt;

typedef struct Hot {
	u32 gen;                  /* bumped by every write this worker takes */
	u64 window;               /* ms an entry stands for; -X, or CFG_HOT_MS */
	u64 decay;                /* ms at which the doorkeeper is cleared */
	u64 hits, fills, admits;  /* reported by /stats */
	u8  door[CFG_HOT_DOOR];
	HotEnt ent[CFG_HOT_SLOTS];
} Hot;

Hot *hot_new(u64 now, u64 window);
void hot_free(Hot *h);

/* A finished response for this key, or NULL.  The caller has already
 * established that the request is one whose answer may be cached. */
const HotEnt *hot_get(Hot *h, u64 hash, const void *k, u32 kl, u64 now);

/* Count one sighting and say whether the key has earned a slot.  Called
 * only after a miss, so a key that is already cached never touches the
 * doorkeeper and the counters describe admissions, not traffic. */
int hot_admit(Hot *h, u64 hash, u64 now);

/* Keep resp as the answer for this key.  doff is where the Date value
 * begins inside it; hot_date_off finds that.  ttl is what the store said
 * the item has left, in ms, or DB_FOREVER style negative for none: an
 * entry may not outlive the item it is a copy of. */
void hot_fill(Hot *h, u64 hash, const void *k, u32 kl, const void *resp,
              u32 rlen, u32 doff, u64 now, i64 ttl);

/* Offset of the Date header's value within a finished response, or -1 if
 * it does not carry one - in which case it must not be cached, since a
 * stale Date is the one thing a proxy in front of us would act on. */
int hot_date_off(const void *resp, u32 rlen);

/* Invalidate the whole set.  Called when this worker takes a write: a
 * generation bump is one store, where hashing the written key to find
 * its slot would cost a hash on every write to retire at most one
 * entry. */
static inline void
hot_dirty(Hot *h)
{
	h->gen++;
}

#endif /* KACHE_HOT_H */
