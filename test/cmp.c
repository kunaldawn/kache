/* kache-cmp - compare two benchmark result files
 *
 * The direction of every metric is carried by its own name, so this tool
 * never needs to know what the benchmarks measure:
 *
 *   ..._per_sec            larger is better
 *   ..._us ..._ns ..._ms   smaller is better
 *   ..._per_op ..._pct     smaller is better
 *   anything else          informational, reported but never judged
 *
 * Adding a metric to a benchmark therefore needs no change here.  A
 * metric that should not be judged simply gets a name without one of
 * those suffixes, which is why the repetition spread is called `spread`
 * and not `spread_pct`.
 *
 * The threshold is a floor, not the whole rule.  Each scenario reports
 * how far its own repetitions spread, and a change smaller than that
 * spread is noise however large the percentage looks, so the two are
 * combined and the larger wins.  Otherwise a machine having a bad minute
 * reads as a regression. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ROWS 512
#define NAME_MAX 96

enum { INFO, HIGHER, LOWER };

typedef struct Row {
	char   name[NAME_MAX];
	double v;
} Row;

typedef struct Set {
	Row    row[MAX_ROWS];
	int    n;
	char   path[256];
} Set;

static int show_all;
/* Five percent, because that is roughly how far the HTTP tier drifts
 * between two runs on a machine that is merely quiet rather than idle.
 * The engine tier is steady to about two, so a real engine change still
 * shows through; lower it with -t on a machine you trust. */
static double thresh = 5.0;

static int
suffix(const char *s, const char *suf)
{
	size_t n = strlen(s), m = strlen(suf);

	return n >= m && !strcmp(s + n - m, suf);
}

static int
direction(const char *name)
{
	if (suffix(name, "_per_sec"))
		return HIGHER;
	if (suffix(name, "_us") || suffix(name, "_ns") || suffix(name, "_ms") ||
	    suffix(name, "_per_op") || suffix(name, "_pct"))
		return LOWER;
	return INFO;
}

static const Row *find(const Set *s, const char *name);

static int
load(Set *s, const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (!f) {
		fprintf(stderr, "kache-cmp: %s: cannot open\n", path);
		return -1;
	}
	snprintf(s->path, sizeof(s->path), "%s", path);
	while (fgets(line, sizeof(line), f)) {
		char *sp;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (!(sp = strchr(line, ' ')))
			continue;
		*sp = '\0';
		if (s->n >= MAX_ROWS)
			break;
		/* A metric can only appear twice if two runs wrote the
		 * same file, and then every value in it is suspect: the
		 * comparison would silently take whichever came first. */
		if (find(s, line)) {
			fprintf(stderr, "kache-cmp: %s: %s appears more than "
			    "once - this file is two runs interleaved, not "
			    "one run\n", path, line);
			fclose(f);
			return -1;
		}
		snprintf(s->row[s->n].name, NAME_MAX, "%.*s", NAME_MAX - 1, line);
		s->row[s->n].v = strtod(sp + 1, NULL);
		s->n++;
	}
	fclose(f);
	return 0;
}

/* How much this scenario's own repetitions varied, averaged over the two
 * runs: the band inside which a difference means nothing. */
static double noise_of(const Set *a, const Set *b, const char *name);

static const Row *
find(const Set *s, const char *name)
{
	int i;

	for (i = 0; i < s->n; i++) {
		if (!strcmp(s->row[i].name, name))
			return &s->row[i];
	}
	return NULL;
}

static double
noise_of(const Set *a, const Set *b, const char *name)
{
	char key[NAME_MAX];
	const char *dot = strrchr(name, '.');
	const Row *ra, *rb;

	if (!dot)
		return 0;
	snprintf(key, sizeof(key), "%.*s.spread", (int)(dot - name), name);
	ra = find(a, key);
	rb = find(b, key);
	return ((ra ? ra->v : 0) + (rb ? rb->v : 0)) / 2;
}

/* metadata lines are strings, so they are compared by hand */
static void
meta_line(const Set *s, const char *tag)
{
	static const char *const keys[] = {
		"meta.tag", "meta.date", "meta.commit", "meta.host"
	};
	char line[512];
	FILE *f = fopen(s->path, "r");
	size_t i;

	printf("%-5s", tag);
	if (!f) {
		printf("\n");
		return;
	}
	for (i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
		rewind(f);
		while (fgets(line, sizeof(line), f)) {
			size_t n = strlen(keys[i]);

			if (strncmp(line, keys[i], n) || line[n] != ' ')
				continue;
			line[strcspn(line, "\n")] = '\0';
			printf("  %s", line + n + 1);
			break;
		}
	}
	fclose(f);
	printf("\n");
}

/* true when the two runs came off different machines, which makes the
 * comparison meaningless and is worth saying loudly */
static int
same_host(const Set *a, const Set *b)
{
	char la[512], lb[512], *pa = NULL, *pb = NULL;
	FILE *fa = fopen(a->path, "r"), *fb = fopen(b->path, "r");
	int same = 1;

	if (fa) {
		while (fgets(la, sizeof(la), fa))
			if (!strncmp(la, "meta.host ", 10))
				pa = la;
		fclose(fa);
	}
	if (fb) {
		while (fgets(lb, sizeof(lb), fb))
			if (!strncmp(lb, "meta.host ", 10))
				pb = lb;
		fclose(fb);
	}
	if (pa && pb)
		same = !strcmp(pa, pb);
	return same;
}

static void
usage(int code)
{
	fprintf(code ? stderr : stdout,
	    "usage: kache-cmp [-a] [-t pct] base.txt new.txt\n"
	    "  -a      include informational metrics\n"
	    "  -t pct  change below this is called noise  (default 5)\n"
	    "\n"
	    "exits non zero when a judged metric regressed by more than the\n"
	    "threshold, so it can gate a change.\n");
	exit(code);
}

int
main(int argc, char *argv[])
{
	Set base, new;
	int better = 0, worse = 0, same = 0, i, opt;

	while ((opt = getopt(argc, argv, "at:h")) != -1) {
		switch (opt) {
		case 'a': show_all = 1; break;
		case 't': thresh = atof(optarg); break;
		default:  usage(opt == 'h' ? 0 : 2);
		}
	}
	if (argc - optind != 2)
		usage(2);
	memset(&base, 0, sizeof(base));
	memset(&new, 0, sizeof(new));
	if (load(&base, argv[optind]) < 0 || load(&new, argv[optind + 1]) < 0)
		return 2;

	meta_line(&base, "base");
	meta_line(&new, "new");
	if (!same_host(&base, &new))
		printf("\nWARNING: different hosts, these numbers are not "
		       "comparable\n");
	printf("\n%-34s %13s %13s %9s\n", "metric", "base", "new", "delta");

	for (i = 0; i < new.n; i++) {
		const char *name = new.row[i].name;
		int dir = direction(name);
		const Row *b;
		double delta, limit;
		const char *verdict;

		if (!strncmp(name, "meta.", 5))
			continue;
		if (dir == INFO && !show_all)
			continue;
		if (!(b = find(&base, name))) {
			if (show_all)
				printf("%-34s %13s %13.0f %9s\n", name, "-",
				       new.row[i].v, "new");
			continue;
		}
		if (dir != INFO && b->v == 0 && new.row[i].v == 0 && !show_all)
			continue;
		delta = b->v != 0 ? (new.row[i].v - b->v) / b->v * 100.0 : 0;
		limit = noise_of(&base, &new, name);
		if (limit < thresh)
			limit = thresh;
		if (dir == INFO)
			verdict = "";
		else if (delta > limit)
			verdict = (dir == HIGHER) ? "better" : "WORSE";
		else if (delta < -limit)
			verdict = (dir == LOWER) ? "better" : "WORSE";
		else if (delta > thresh || delta < -thresh)
			verdict = "~noise";
		else
			verdict = "";
		if (dir != INFO) {
			if (!*verdict || !strcmp(verdict, "~noise"))
				same++;
			else if (!strcmp(verdict, "better"))
				better++;
			else
				worse++;
		}
		printf("%-34s %13.0f %13.0f %+8.1f%% %s\n", name, b->v,
		       new.row[i].v, delta, verdict);
	}
	printf("\n%d better, %d worse, %d unchanged "
	       "(noise floor %.1f%%, widened per scenario)\n",
	       better, worse, same, thresh);
	return worse > 0;
}
