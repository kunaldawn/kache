/* kache - the cached clock and the preformatted HTTP date, both
 * refreshed by whichever worker comes back from epoll_wait next, so no
 * request path calls into time and no thread exists only to tick.  The
 * price is staleness bounded by one epoll timeout. */
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "util/clk.h"

_Atomic u64 clk_ms;

/* Two buffers, flipped once a second: a reader either sees the previous
 * second or the current one, never a half written string.  That scheme
 * wants a single writer, and every worker crosses the second boundary at
 * about the same moment, so date_busy hands the rebuild to exactly one
 * of them and the rest go straight back to work.  Nobody ever waits on
 * it: a thread that does not get it has nothing to contribute. */
static char date_buf[2][CLK_DATE_LEN + 3];
static _Atomic unsigned date_slot;
static _Atomic unsigned date_busy;
static _Atomic u64 date_sec;

static const char wday[7][4] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char mon[12][4] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

u64
clk_read_ms(void)
{
	struct timespec ts;

#ifdef CLOCK_REALTIME_COARSE
	if (clock_gettime(CLOCK_REALTIME_COARSE, &ts) != 0)
#endif
		clock_gettime(CLOCK_REALTIME, &ts);
	return (u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull;
}

static void
put2(char *d, unsigned v)
{
	d[0] = (char)('0' + (v / 10) % 10);
	d[1] = (char)('0' + v % 10);
}

static void
date_format(char *d, time_t t)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	memcpy(d, wday[tm.tm_wday % 7], 3);
	d[3] = ',';
	d[4] = ' ';
	put2(d + 5, (unsigned)tm.tm_mday);
	d[7] = ' ';
	memcpy(d + 8, mon[tm.tm_mon % 12], 3);
	d[11] = ' ';
	put2(d + 12, (unsigned)((tm.tm_year + 1900) / 100));
	put2(d + 14, (unsigned)((tm.tm_year + 1900) % 100));
	d[16] = ' ';
	put2(d + 17, (unsigned)tm.tm_hour);
	d[19] = ':';
	put2(d + 20, (unsigned)tm.tm_min);
	d[22] = ':';
	put2(d + 23, (unsigned)tm.tm_sec);
	memcpy(d + 25, " GMT", 4);
}

void
clk_update(void)
{
	u64 ms = clk_read_ms();
	u64 cur = atomic_load_explicit(&clk_ms, memory_order_relaxed);
	u64 sec, seen;
	unsigned idle = 0;

	/* Every worker publishes this, so one that was descheduled between
	 * reading the clock and arriving here would otherwise drag the
	 * cache back over a newer value.  conn.c orders its idle list by
	 * this number with unsigned arithmetic, so a step back unsorts the
	 * list and leaves idle connections unreaped.  A reading far enough
	 * behind is either a long stall or the clock being set, and one
	 * more read tells those apart: a stall reads current, a set does
	 * not. */
	if (UNLIKELY(ms + 1000 < cur))
		ms = clk_read_ms();
	while ((ms > cur || ms + 1000 < cur) &&
	       !atomic_compare_exchange_weak_explicit(&clk_ms, &cur, ms,
	           memory_order_relaxed, memory_order_relaxed))
		;

	sec = ms / 1000;
	seen = atomic_load_explicit(&date_sec, memory_order_relaxed);
	if (LIKELY(sec == seen))
		return;
	/* Workers straddle the boundary: one reads the new second a moment
	 * before another reads the old one.  Exactly one second backwards
	 * is that race, and rebuilding on it would have the two take turns
	 * dragging the date to and fro; anything further back is the clock
	 * being set, and that we follow.  The test is written the narrow
	 * way on purpose - `sec < seen` reads as equivalent and would
	 * freeze the Date for good after a step back. */
	if (sec + 1 == seen)
		return;
	if (!atomic_compare_exchange_strong_explicit(&date_busy, &idle, 1,
	    memory_order_acquire, memory_order_relaxed))
		return;
	/* Under the claim now, so re-ask: the winner of the previous
	 * second may have published while this thread was on its way in. */
	seen = atomic_load_explicit(&date_sec, memory_order_relaxed);
	if (sec != seen && sec + 1 != seen) {
		unsigned slot = atomic_load_explicit(&date_slot,
		                    memory_order_relaxed) ^ 1u;

		/* Only the holder writes date_slot, and the acquire above
		 * pairs with the previous holder's release, so this reads
		 * the slot that is really published and writes the other
		 * one - never the one a reader is pointed at. */
		date_format(date_buf[slot], (time_t)sec);
		atomic_store_explicit(&date_sec, sec, memory_order_relaxed);
		atomic_store_explicit(&date_slot, slot,
		                      memory_order_release);
	}
	atomic_store_explicit(&date_busy, 0, memory_order_release);
}

void
clk_init(void)
{
	atomic_store_explicit(&date_sec, 0, memory_order_relaxed);
	atomic_store_explicit(&date_busy, 0, memory_order_relaxed);
	/* so clk_date() is answerable before the first rebuild, whatever
	 * order the workers come up in */
	date_format(date_buf[0], (time_t)(clk_read_ms() / 1000));
	atomic_store_explicit(&date_slot, 0, memory_order_relaxed);
	clk_update();
}

const char *
clk_date(void)
{
	unsigned cur = atomic_load_explicit(&date_slot, memory_order_acquire);

	return date_buf[cur];
}
