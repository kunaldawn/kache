/* kache - the cached clock and the preformatted HTTP date, both
 * refreshed by the ticker thread so no request path calls into time. */
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "util/clk.h"

_Atomic u64 clk_ms;

/* Two buffers, flipped by the ticker: a reader either sees the previous
 * second or the current one, never a half written string. */
static char date_buf[2][CLK_DATE_LEN + 3];
static _Atomic unsigned date_slot;
static u64 date_sec;

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
	u64 sec = ms / 1000;

	atomic_store_explicit(&clk_ms, ms, memory_order_relaxed);
	if (sec != date_sec) {
		unsigned cur = atomic_load_explicit(&date_slot,
		                                    memory_order_relaxed);
		date_sec = sec;
		date_format(date_buf[cur ^ 1u], (time_t)sec);
		atomic_store_explicit(&date_slot, cur ^ 1u,
		                      memory_order_release);
	}
}

void
clk_init(void)
{
	date_sec = 0;
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
