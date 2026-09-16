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

/* hash table load factor limit, in 256ths (192/256 = 0.75) */
#define CFG_LOAD_LIMIT       192u

/* ---- eviction ------------------------------------------------------- */

/* candidates inspected per eviction; larger is a better LRU approximation */
#define CFG_EVICT_SAMPLES    8u
/* how many eviction rounds an allocation may trigger before giving up */
#define CFG_EVICT_ROUNDS     64u

/* ---- time ----------------------------------------------------------- */

/* default ttl applied when a request does not carry one, 0 = never */
#define CFG_TTL_MS           0ll
/* coarse clock resolution */
#define CFG_TICK_MS          1u
/* msync interval, 0 disables background flushing */
#define CFG_SYNC_MS          1000u

/* ---- server --------------------------------------------------------- */

#define CFG_ADDR             "0.0.0.0"
#define CFG_PORT             "7070"
#define CFG_BACKLOG          1024
#define CFG_THREADS          0u            /* 0 = one per cpu */
#define CFG_CONNS            8192u         /* per thread */
#define CFG_IDLE_MS          60000u        /* idle connection timeout */
#define CFG_EVENTS           256           /* epoll batch */

/* connection buffers: start small, grow on demand, shrink back when a
 * request leaves them oversized */
#define CFG_BUF_INIT         2048u
#define CFG_BUF_KEEP         16384u
/* headroom for the request line and headers on top of the largest value;
 * together they bound how much one connection may buffer */
#define CFG_REQ_SLACK        8192u
/* stop parsing pipelined requests once this much response is queued */
#define CFG_OUT_HIGH         262144u

/* spins before a contended lock parks in the kernel */
#define CFG_LOCK_SPINS       128

#endif /* KACHE_CONFIG_H */
