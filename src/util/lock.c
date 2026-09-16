/* kache - the slow paths of the shard mutex: spin, then park on a futex.
 * The fast paths are inlined in lock.h. */
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "config.h"
#include "util/lock.h"

static inline long
futex(void *addr, int op, u32 val)
{
	return syscall(SYS_futex, addr, op | FUTEX_PRIVATE_FLAG, val,
	               NULL, NULL, 0);
}

void
lock_slow(Lock *l)
{
	u32 v;
	int i;

	for (i = 0; i < CFG_LOCK_SPINS; i++) {
		if (atomic_load_explicit(l, memory_order_relaxed) == 0) {
			u32 e = 0;
			if (atomic_compare_exchange_weak_explicit(l, &e, 1,
			    memory_order_acquire, memory_order_relaxed))
				return;
		}
		cpu_relax();
	}
	/* Claim the lock as contended.  If it was free we own it, otherwise
	 * we park; the 2 we just stored makes the owner wake us. */
	while ((v = atomic_exchange_explicit(l, 2, memory_order_acquire)) != 0)
		futex(l, FUTEX_WAIT, 2);
}

void
lock_wake(Lock *l)
{
	futex(l, FUTEX_WAKE, 1);
}
