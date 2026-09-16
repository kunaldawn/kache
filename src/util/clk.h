/* kache - coarse clock
 *
 * Expiry only needs millisecond resolution, so the hot paths read a
 * cached timestamp instead of entering the vDSO on every request.
 * Every worker refreshes it once around its event loop, so the cache
 * trails the real clock by at most one epoll timeout even when no
 * request arrives at all, and by nothing worth measuring under load. */
#ifndef KACHE_CLK_H
#define KACHE_CLK_H

#include <stdatomic.h>

#include "util/util.h"

#define CLK_DATE_LEN 29   /* "Sun, 06 Nov 1994 08:49:37 GMT" */

extern _Atomic u64 clk_ms;

/* wall clock milliseconds, cached */
static inline u64
now_ms(void)
{
	return atomic_load_explicit(&clk_ms, memory_order_relaxed);
}

/* wall clock seconds, cached, truncated to 32 bits */
static inline u32
now_sec(void)
{
	return (u32)(now_ms() / 1000);
}

u64  clk_read_ms(void);           /* uncached */
void clk_init(void);              /* once, before any worker starts */
void clk_update(void);            /* refresh cache; any thread, any time */
const char *clk_date(void);       /* CLK_DATE_LEN bytes, not terminated */

#endif /* KACHE_CLK_H */
