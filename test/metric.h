/* kache - shared plumbing for the benchmark tools
 *
 * Both benchmarks measure the same way and report the same way, so the
 * driver can concatenate their output and the comparison tool can read
 * it without knowing which one produced a line.
 *
 * A result line is
 *
 *     <scenario>.<metric> <value>
 *
 * and the metric suffix carries its own direction: anything ending in
 * _per_sec is better when larger, anything ending in _us, _ns, _per_op
 * or _pct is better when smaller.  That one convention is all kache-cmp
 * needs to know, so adding a metric never means editing the comparison
 * tool. */
#ifndef KACHE_METRIC_H
#define KACHE_METRIC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* kache-bench links nothing from the tree, so it gets the short names
 * here rather than by depending on the store's headers.  C11 allows the
 * repeat when kache-micro pulls in util.h as well. */
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;

/* Every scenario is run several times.  Throughput is reported as the
 * best of those runs, not the mean or the median, because interference
 * is one sided: another process, a migration or a frequency dip can only
 * ever make a run slower, never faster.  The best run is therefore the
 * one least disturbed, and the cleanest thing to compare a later build
 * against.  Rates that describe correctness rather than speed - miss and
 * error percentages - use the median instead, since there "best" would
 * mean flattering.
 *
 * The spread across the runs is reported alongside and is the honest
 * error bar: a scenario that varied by 20% is not evidence of anything,
 * and kache-cmp widens its threshold by exactly that much. */
#define REPS_MAX 15

static int metric_machine;   /* -m: emit result lines instead of prose */

static inline double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline int
dcmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static inline double
median(const double *v, int n)
{
	double s[REPS_MAX];

	if (n <= 0)
		return 0;
	if (n > REPS_MAX)
		n = REPS_MAX;
	memcpy(s, v, (size_t)n * sizeof(*s));
	qsort(s, (size_t)n, sizeof(*s), dcmp);
	return (n & 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
}

/* how far apart the repetitions landed, as a percentage of the median */
static inline double
spread_pct(const double *v, int n)
{
	double lo, hi, mid;
	int i;

	if (n < 2)
		return 0;
	lo = hi = v[0];
	for (i = 1; i < n; i++) {
		if (v[i] < lo)
			lo = v[i];
		if (v[i] > hi)
			hi = v[i];
	}
	mid = median(v, n);
	return mid > 0 ? (hi - lo) / mid * 100.0 : 0;
}

static inline double
best(const double *v, int n)
{
	double hi;
	int i;

	if (n <= 0)
		return 0;
	hi = v[0];
	for (i = 1; i < n; i++) {
		if (v[i] > hi)
			hi = v[i];
	}
	return hi;
}

/* the mirror of best(), for metrics where smaller is the good direction */
static inline double
least(const double *v, int n)
{
	double lo;
	int i;

	if (n <= 0)
		return 0;
	lo = v[0];
	for (i = 1; i < n; i++) {
		if (v[i] < lo)
			lo = v[i];
	}
	return lo;
}

static inline void
metric(const char *scen, const char *name, double v)
{
	if (!metric_machine)
		return;
	printf("%s.%s %.3f\n", scen, name, v);
}

/* the human readable line, printed instead of the metrics */
static inline void
report(const char *scen, double ops, double spread, const char *extra)
{
	if (metric_machine)
		return;
	printf("  %-16s %12.0f ops/s  %8.1f ns/op  +-%.1f%%%s%s\n",
	       scen, ops, ops > 0 ? 1e9 / ops : 0, spread,
	       extra && *extra ? "  " : "", extra ? extra : "");
	fflush(stdout);
}

#endif /* KACHE_METRIC_H */
