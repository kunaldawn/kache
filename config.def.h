/* kache - compile time configuration.
 * Copy of config.def.h; edit config.h to taste and rebuild. */
#ifndef KACHE_CONFIG_H
#define KACHE_CONFIG_H

/* ---- store geometry ------------------------------------------------- */

/* default backing file and its size (the file is created sparse) */
#define CFG_PATH             "kache.db"
#define CFG_SIZE             (1024ull << 20)

/* hard limits of a single item */
#define CFG_MAX_KEY          255u          /* bytes, <= 65535 */
#define CFG_MAX_VAL          (256u << 10)  /* bytes */

/* expected size of a stored item, header and key included, rounded up to
 * the allocator grain.  It only steers the split between index and data:
 * enough buckets are provisioned that an arena full of items this size
 * can still be indexed at the load factor below.  Set it too high and the
 * index fills while the arena is half empty; too low and the index costs
 * memory that stores nothing.  Neither breaks anything. */
#define CFG_AVG_ITEM         128u

/* a shard is the unit of locking; more shards means less contention and
 * a smaller arena each.  0 selects a value from the file size. */
#define CFG_SHARDS           0u
#define CFG_SHARDS_MIN       8u
#define CFG_SHARDS_MAX       4096u

/* a shard's arena must hold this many maximum sized items */
#define CFG_ARENA_ITEMS      16u

/* most keys one batch request may carry; the body is bounded by
 * CFG_MAX_VAL + CFG_REQ_SLACK as well, whichever bites first */
#define CFG_BATCH_MAX        1024u

/* hash table load factor limit, in 256ths (192/256 = 0.75) */
#define CFG_LOAD_LIMIT       192u

/* ---- containers ------------------------------------------------------ */

/* A nested map is open addressed like the shard index, at the same load
 * factor.  Its slots are 8 bytes, so the smallest table is one cache
 * line and covers a map of six fields without ever growing. */
#define CFG_KKV_SLOTS_MIN    8u
#define CFG_KKV_SLOTS_MAX    (1u << 26)
#define CFG_KKV_LOAD         192u

/* Queue segments start here and double up to the ceiling, so a queue of
 * five messages does not pay for a queue of five million and a queue of
 * five million does not allocate every few pushes.  An entry too large
 * for the ceiling gets a segment of its own. */
#define CFG_QSEG_MIN         512u
#define CFG_QSEG_MAX         (32u << 10)
/* an empty segment no larger than this is kept for the next push, so a
 * queue that hovers around empty does not allocate on every message */
#define CFG_QSEG_KEEP        4096u

/* Entries one pop or one peek may return, and how many expired ones a
 * read may clear off the ends before getting on with its work. */
#define CFG_QPOP_MAX         4096u
#define CFG_QEXPIRE_BUDGET   32u

/* Sub blocks the sweeper reclaims on the way into an operation that may
 * allocate.  Deleting a container is constant time because the work of
 * dismantling it is spread over the requests that follow, a few blocks
 * at a time; this is how many.  Larger reclaims memory sooner and costs
 * the unlucky request more. */
#define CFG_SWEEP_BUDGET     64u

/* The same work, done by a worker's once a second housekeeping tick
 * rather than by a request, so a shard nobody is asking about still
 * gives its memory back.  It takes the shard lock with a try, never a
 * wait: a busy shard is one whose own traffic is already sweeping it. */
#define CFG_RECLAIM_BUDGET   4096u

/* fields or entries one batch request may carry, and how many bytes one
 * enumeration may answer with.  Both bound how long a shard lock is held
 * by a single request, which is the real reason they exist. */
#define CFG_CONT_BATCH_MAX   4096u
#define CFG_CONT_DUMP_MAX    (8u << 20)

/* ---- hot keys -------------------------------------------------------- */

/* One key can carry a disproportionate share of the load, and when it
 * does it behaves unlike every other key in the store.  Measured on the
 * engine tier, a single key answers 4.5x faster than a spread keyspace
 * on one thread - the record, its bucket and its lock word never leave
 * L1 - and then collapses to a quarter of that on eight, because one
 * exclusive lock and one cache line are all there is to share.  The
 * answer is not a cleverer lock.  It is to stop reaching the shard at
 * all: each worker keeps its own small set of finished responses for
 * the keys that have proved hot, and answers them with a memcpy and a
 * patched Date, touching nothing another core can see.
 *
 * The price is staleness bounded by CFG_HOT_MS: a write is invisible to
 * the other workers for that long.  The worker that takes the write
 * drops its own set at once, so a client on one connection still reads
 * its own writes.  An entry never outlives the item's own ttl, whichever
 * of the two is shorter, because an expiry is a promise and staleness is
 * only a trade.  0 disables all of it. */
#define CFG_HOT_MS           0u
/* responses kept per worker, a power of two; direct mapped by hash */
#define CFG_HOT_SLOTS        16u
/* longest key that may be cached.  A longer one is never hot enough to
 * be worth the bytes, and the cap is what keeps an entry compact. */
#define CFG_HOT_KEY          64u
/* largest finished response that may be cached */
#define CFG_HOT_RESP         1024u
/* Admission.  Caching on first sight would thrash the set on any real
 * keyspace and pay the copy for keys that are never asked for twice, so
 * a key has to be seen CFG_HOT_ADMIT times within one decay window
 * before it is allowed in.  The counters are one byte each, indexed by
 * hash, and shared between keys that collide - a doorkeeper, not an
 * accounting record.  Clearing the whole array once per window is the
 * decay; it costs a memset of CFG_HOT_DOOR bytes a second. */
#define CFG_HOT_DOOR         4096u
#define CFG_HOT_ADMIT        32u
#define CFG_HOT_DECAY_MS     1000u

/* ---- cluster --------------------------------------------------------- */

/* How often a node ships what it has written to its peers.  This is the
 * coalescing window, and it is the only thing that sets peer traffic: a
 * key written a million times inside one window is sent once, carrying
 * the value it ended up with.  Lower is fresher and chattier; the number
 * that matters is that peer traffic is O(hot keys / interval) and owes
 * nothing to the write rate.
 *
 * It is also the staleness bound.  A write is visible on the node that
 * took it at once and on the others after one window plus a round
 * trip. */
#define CFG_REPL_MS          50u

/* How often a sharded node re-resolves its -J name to find out who its
 * peers are now.  This is the whole reaction time of the cluster to a
 * scale event: a pod that appears is routed to within this long, and a
 * pod that goes away stops being redirected to within it.  Kubernetes
 * withdraws a pod's A record when its readiness probe fails, so the DNS
 * answer leads the pod's departure rather than trailing it, which is
 * what makes a short interval useful rather than merely busy.
 *
 * A resolve that finds the same members costs one getaddrinfo and
 * nothing else - the owner table is only rebuilt when the membership
 * actually moved. */
#define CFG_RESOLVE_MS       2000u

/* ---- eviction ------------------------------------------------------- */

/* candidates inspected per eviction; larger is a better LRU approximation */
#define CFG_EVICT_SAMPLES    8u
/* how many eviction rounds an allocation may trigger before giving up */
#define CFG_EVICT_ROUNDS     64u

/* seconds a record's atime may lag the clock before a read stamps it
 * again.  Sampled eviction only compares atimes with each other, so slack
 * this small cannot change which sample looks oldest, and it keeps a
 * plain GET from dirtying the record, and its page, on every hit.
 * 0 restores the unconditional stamp. */
#define CFG_ATIME_SLACK      4u

/* ---- time ----------------------------------------------------------- */

/* default ttl applied when a request does not carry one, 0 = never */
#define CFG_TTL_MS           0ll
/* msync interval, 0 disables background flushing */
#define CFG_SYNC_MS          1000u

/* ---- server --------------------------------------------------------- */

#define CFG_ADDR             "0.0.0.0"
#define CFG_PORT             "7070"
#define CFG_BACKLOG          1024
#define CFG_THREADS          0u            /* 0: see CFG_THREADS_SMT */
#define CFG_CONNS            8192u         /* per thread */
#define CFG_IDLE_MS          60000u        /* idle connection timeout */
#define CFG_EVENTS           256           /* epoll batch */

/* what CFG_THREADS 0 means: 1 counts every logical cpu, 0 counts physical
 * cores only.  Hyperthread siblings share one cache and one set of
 * load/store units, and this engine spends its time waiting on memory, so
 * the second worker on a core can cost more than it earns. */
#define CFG_THREADS_SMT      1
/* pin each worker to one cpu.  Its buffers and connection state then stay
 * on the core that faulted them in; on a machine shared with other work,
 * letting the scheduler move threads is the better trade.  Assumes one
 * worker per logical cpu, so it pairs with CFG_THREADS_SMT 1. */
#define CFG_AFFINITY         0
/* how often an active connection is moved back to the young end of the
 * idle list.  Only the idle reaper reads that order, so coarseness here
 * merely blurs CFG_IDLE_MS by this much and spares the relink on almost
 * every event.  0 relinks on every event. */
#define CFG_TOUCH_MS         1000u

/* connection buffers: start small, grow on demand, shrink back when a
 * request leaves them oversized */
#define CFG_BUF_INIT         2048u
#define CFG_BUF_KEEP         16384u
/* headroom for the request line and headers on top of the largest value;
 * together they bound how much one connection may buffer */
#define CFG_REQ_SLACK        8192u
/* stop parsing pipelined requests once this much response is queued */
#define CFG_OUT_HIGH         262144u

/* minimal responses drop the headers a cache client rarely reads back:
 * the Server line, Connection: keep-alive, and on a successful read of a
 * key the Content-Type plus the ETag / X-Kache-TTL / X-Kache-Flags trio.
 * Writes keep all of those, so conditional requests and CAS still work.
 * What you give up is read side metadata, and a 64 byte GET answer
 * shrinks from 265 bytes to 140.  -M turns it on for one run. */
#define CFG_MINIMAL          0

/* spins before a contended lock parks in the kernel */
#define CFG_LOCK_SPINS       128

#endif /* KACHE_CONFIG_H */
