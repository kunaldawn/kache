/* kache - futex backed mutex
 *
 * Critical sections are a hash probe and a memcpy, so contention is rare
 * and short.  An exclusive lock over many shards beats a reader/writer
 * lock here: even a read touches the record's access time, so shared
 * acquisition would dirty the same cache line anyway.
 *
 * Three states, the classic Drepper design:
 *   0  unlocked
 *   1  locked, no waiters
 *   2  locked, waiters parked in the kernel */
#ifndef KACHE_LOCK_H
#define KACHE_LOCK_H

#include <stdatomic.h>

#include "util/util.h"

typedef _Atomic u32 Lock;

void lock_slow(Lock *l);
void lock_wake(Lock *l);

static inline void
lock_init(Lock *l)
{
	atomic_store_explicit(l, 0, memory_order_relaxed);
}

static inline void
lock_acquire(Lock *l)
{
	u32 e = 0;

	if (LIKELY(atomic_compare_exchange_strong_explicit(l, &e, 1,
	    memory_order_acquire, memory_order_relaxed)))
		return;
	lock_slow(l);
}

static inline int
lock_try(Lock *l)
{
	u32 e = 0;

	return atomic_compare_exchange_strong_explicit(l, &e, 1,
	    memory_order_acquire, memory_order_relaxed);
}

static inline void
lock_release(Lock *l)
{
	if (UNLIKELY(atomic_exchange_explicit(l, 0, memory_order_release) == 2))
		lock_wake(l);
}

#endif /* KACHE_LOCK_H */
