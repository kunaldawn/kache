/* kache - per worker counters
 *
 * Each worker owns a cache line of its own and is the only writer to it,
 * so counting is a plain load/add/store with no bus locking and no
 * sharing between threads.  The fields are relaxed atomics all the same:
 * /stats reads them from another thread, and a plain read next to a
 * plain write is a data race however harmless the outcome would be. */
#ifndef KACHE_STATS_H
#define KACHE_STATS_H

#include <stdatomic.h>

#include "util/util.h"

typedef _Atomic u64 Counter;

typedef struct Stats {
	Counter requests;
	Counter hits;
	Counter misses;
	Counter sets;
	Counter dels;
	Counter incrs;
	Counter cats;
	Counter touches;
	Counter errors;
	Counter accepted;
	Counter closed;
	Counter current;
	Counter bytes_in;
	Counter bytes_out;
	Counter kkv_reads;
	Counter kkv_writes;
	Counter kkv_dels;
	Counter q_pushes;
	Counter q_pops;
	u8      pad[192 - 19 * 8];
} Stats;

_Static_assert(sizeof(Stats) == 192, "stats must stay three cache lines");

/* Single writer, so a relaxed load and store is enough; no read/modify/
 * write instruction, no contention with the other workers. */
static inline void
st_add(Counter *c, u64 n)
{
	atomic_store_explicit(c,
	    atomic_load_explicit(c, memory_order_relaxed) + n,
	    memory_order_relaxed);
}

static inline void
st_inc(Counter *c)
{
	st_add(c, 1);
}

static inline void
st_dec(Counter *c)
{
	st_add(c, (u64)-1);
}

static inline u64
st_get(const Counter *c)
{
	return atomic_load_explicit(c, memory_order_relaxed);
}

void   stats_init(unsigned n);
void   stats_fini(void);
Stats *stats_of(unsigned i);
/* sum every worker's counters into a plain snapshot */
void   stats_sum(u64 *out, unsigned n);

#define STATS_FIELDS 19

#endif /* KACHE_STATS_H */
