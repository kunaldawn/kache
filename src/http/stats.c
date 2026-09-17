/* kache - allocation and aggregation of the per worker counters. */
#include <stdlib.h>
#include <string.h>

#include "http/stats.h"

static Stats *all;
static unsigned count;

void
stats_init(unsigned n)
{
	if (posix_memalign((void **)&all, 64, (size_t)n * sizeof(Stats)) != 0)
		die("out of memory");
	memset(all, 0, (size_t)n * sizeof(Stats));
	count = n;
}

void
stats_fini(void)
{
	free(all);
	all = NULL;
	count = 0;
}

Stats *
stats_of(unsigned i)
{
	return &all[i];
}

/* The workers keep counting while this runs, so the snapshot is a blend
 * of instants rather than one instant.  For monotonic counters that is
 * the difference between "a moment ago" and "right now", which is all a
 * scrape can ever claim anyway. */
void
stats_sum(u64 *out, unsigned n)
{
	unsigned i, j;

	if (n > STATS_FIELDS)
		n = STATS_FIELDS;
	memset(out, 0, n * sizeof(*out));
	for (i = 0; i < count; i++) {
		const Counter *src = (const Counter *)&all[i];

		for (j = 0; j < n; j++)
			out[j] += st_get(&src[j]);
	}
}
