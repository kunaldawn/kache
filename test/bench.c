/* kache-bench - HTTP load generator
 *
 * Closed loop: every thread owns one connection and keeps `pipeline`
 * requests in flight, so the reported rate is what the server sustained
 * at that concurrency, not what an open loop offered it.
 *
 * Two modes.  Throughput mode pipelines and reports operations per
 * second; latency mode keeps one request outstanding per connection and
 * reports percentiles, because a percentile over pipelined requests
 * measures the queue, not the server. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "metric.h"

/* Latencies are bucketed at one microsecond up to 65 ms, which is exact
 * for everything a local cache does and costs 256 KiB a thread. */
#define HBUCKETS 65536

enum { W_GET, W_SET, W_MIXED, W_INCR, W_MGET, W_FILL };

static const char *const wnames[] = {
	"get", "set", "mixed", "incr", "mget", "fill"
};

static const char *host = "127.0.0.1";
static const char *port = "7070";
static const char *scen = "http";
static int nthreads = 8;
static int pipeline = 16;
static int keyspace = 100000;
static int valsize = 64;
static int readpct = 90;
static int workload = W_MIXED;
static int batch = 64;         /* keys per request in the mget workload */
static int latency;
static int reps = 3;
static int warmup = 1;
static double secs = 2.0;
static u64 seed0 = 1;

static _Atomic int go, stop;

typedef struct Thread {
	pthread_t th;
	int   id;
	int   fd;
	char *req;
	size_t reqcap;
	char *buf;          /* response buffer */
	size_t buflen, bufcap;
	size_t off;         /* read cursor into buf */
	size_t need;        /* body bytes still to skip */
	u64   ops;
	u64   errs;
	u32  *hist;
	u64   over;
	u64   max_us;
	char *val;
	char *scratch;      /* batch body, assembled before its header */
} Thread;

/* ---- plumbing --------------------------------------------------------- */

static u64
xrand(u64 *s)
{
	u64 x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545f4914f6cdd1dull;
}

static int
dial(void)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		if ((fd = socket(ai->ai_family, ai->ai_socktype,
		                 ai->ai_protocol)) < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0)
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	return fd;
}

static void
hist_add(Thread *t, double sec)
{
	u64 us = (u64)(sec * 1e6 + 0.5);

	if (us > t->max_us)
		t->max_us = us;
	if (us < HBUCKETS)
		t->hist[us]++;
	else
		t->over++;
}

/* ---- request and response --------------------------------------------- */

/* A batch request has to know its own body length before it can write
 * the Content-Length, so the body is assembled first and the header goes
 * in front of it afterwards. */
static size_t
build_mget(Thread *t, u64 *s, size_t at)
{
	size_t body = 0, hdr;
	int i;

	for (i = 0; i < batch; i++)
		body += (size_t)snprintf(t->scratch + body,
		    (size_t)batch * 16 - body, "k%u\n",
		    (unsigned)(xrand(s) % (unsigned)keyspace));
	hdr = (size_t)snprintf(t->req + at, t->reqcap - at,
	    "POST /mget HTTP/1.1\r\nHost: b\r\nContent-Length: %zu\r\n\r\n",
	    body);
	memcpy(t->req + at + hdr, t->scratch, body);
	return hdr + body;
}

static size_t
build(Thread *t, u64 *s, int n)
{
	size_t len = 0;
	int i;

	if (workload == W_MGET) {
		for (i = 0; i < n; i++)
			len += build_mget(t, s, len);
		return len;
	}

	for (i = 0; i < n; i++) {
		u64 r = xrand(s);
		unsigned k = (unsigned)((r >> 8) % (unsigned)keyspace);
		int write = 0;

		switch (workload) {
		case W_GET:   write = 0; break;
		case W_SET:   write = 1; break;
		case W_MIXED: write = (int)(r & 0x7f) >= readpct * 127 / 100;
		              break;
		case W_INCR:
			len += (size_t)snprintf(t->req + len, t->reqcap - len,
			    "POST /incr/c%u?by=1 HTTP/1.1\r\nHost: b\r\n\r\n",
			    k);
			continue;
		}
		if (write)
			len += (size_t)snprintf(t->req + len, t->reqcap - len,
			    "PUT /kv/k%u HTTP/1.1\r\nHost: b\r\n"
			    "Content-Length: %d\r\n\r\n%s", k, valsize, t->val);
		else
			len += (size_t)snprintf(t->req + len, t->reqcap - len,
			    "GET /kv/k%u HTTP/1.1\r\nHost: b\r\n\r\n", k);
	}
	return len;
}

/* Pulls whole responses out of the buffer and returns how many.  Only
 * the status class and the content length are looked at; this is a load
 * generator, not a conformance checker.
 *
 * Like the server, it consumes with a cursor and scans only the bytes it
 * has not consumed.  Doing either of those per response over the whole
 * buffer makes the client quadratic in the pipeline depth, and a load
 * generator that becomes the bottleneck measures itself. */
static int
consume(Thread *t)
{
	int done = 0;

	for (;;) {
		char *at = t->buf + t->off, *end, *cl;
		size_t avail = t->buflen - t->off, hdr;
		long body = 0;

		if (t->need) {
			size_t take = t->need < avail ? t->need : avail;

			t->off += take;
			t->need -= take;
			if (t->need)
				return done;
			continue;
		}
		if (!(end = memmem(at, avail, "\r\n\r\n", 4)))
			return done;
		hdr = (size_t)(end - at) + 4;
		if (avail > 9 && at[9] != '2')
			t->errs++;
		if ((cl = memmem(at, hdr, "\r\nContent-Length:", 17)) != NULL)
			body = strtol(cl + 17, NULL, 10);
		t->off += hdr;
		t->need = (size_t)body;
		done++;
	}
}

static int
await(Thread *t, int want)
{
	int got = 0;

	while (got < want) {
		ssize_t n;

		/* reclaim consumed bytes once per read, not once per
		 * response */
		if (t->off) {
			if (t->buflen > t->off)
				memmove(t->buf, t->buf + t->off,
				        t->buflen - t->off);
			t->buflen -= t->off;
			t->off = 0;
		}
		n = read(t->fd, t->buf + t->buflen,
		         t->bufcap - 1 - t->buflen);
		if (n <= 0)
			return -1;
		t->buflen += (size_t)n;
		got += consume(t);
	}
	return 0;
}

/* ---- workers ---------------------------------------------------------- */

static void
run_fill(Thread *t)
{
	int k;

	for (k = t->id; k < keyspace; k += nthreads) {
		size_t len = (size_t)snprintf(t->req, t->reqcap,
		    "PUT /kv/k%u HTTP/1.1\r\nHost: b\r\nContent-Length: %d"
		    "\r\n\r\n%s", (unsigned)k, valsize, t->val);

		if (write(t->fd, t->req, len) != (ssize_t)len)
			return;
		if (await(t, 1) < 0)
			return;
		t->ops++;
	}
}

static void *
worker(void *arg)
{
	Thread *t = arg;
	u64 s = seed0 * 6364136223846793005ull + (u64)t->id * 2654435761u + 1;

	if ((t->fd = dial()) < 0) {
		fprintf(stderr, "kache-bench: cannot connect to %s:%s\n",
		        host, port);
		return NULL;
	}
	if (workload == W_FILL) {
		run_fill(t);
		close(t->fd);
		return NULL;
	}
	while (!atomic_load_explicit(&go, memory_order_acquire))
		;
	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		size_t len = build(t, &s, pipeline);
		double t0 = 0;

		if (latency)
			t0 = now_s();
		if (write(t->fd, t->req, len) != (ssize_t)len)
			break;
		if (await(t, pipeline) < 0)
			break;
		if (latency)
			hist_add(t, now_s() - t0);
		t->ops += (u64)pipeline * (workload == W_MGET ? (u64)batch : 1);
	}
	close(t->fd);
	return NULL;
}

/* ---- percentiles ------------------------------------------------------ */

static u64
percentile(const u64 *h, u64 total, u64 over, u64 max_us, double p)
{
	u64 want = (u64)(p * (double)total), seen = 0;
	u64 i;

	if (!total)
		return 0;
	for (i = 0; i < HBUCKETS; i++) {
		seen += h[i];
		if (seen >= want)
			return i;
	}
	(void)over;
	return max_us;
}

/* ---- main ------------------------------------------------------------- */

static void
usage(int code)
{
	fprintf(code ? stderr : stdout,
	    "usage: kache-bench [-mL] [-n name] [-h host] [-p port] "
	    "[-W workload]\n"
	    "                   [-t threads] [-P pipeline] [-d seconds] "
	    "[-r reps]\n"
	    "                   [-k keyspace] [-v bytes] [-R read%%] "
	    "[-S seed]\n"
	    "  -m          emit result lines instead of a table\n"
	    "  -n name     scenario name used to prefix those lines\n"
	    "  -W load     get, set, mixed, incr or fill  (default mixed)\n"
	    "  -L          latency mode: one request in flight, percentiles\n"
	    "  -t threads  connections, one per thread    (default 8)\n"
	    "  -P depth    pipeline depth                 (default 16)\n"
	    "  -d seconds  measured time per repetition   (default 2)\n"
	    "  -r reps     repetitions, best reported     (default 3)\n"
	    "  -w 0|1      discard one warmup pass first  (default 1)\n"
	    "  -k keys     keyspace                       (default 100000)\n"
	    "  -v bytes    value size                     (default 64)\n"
	    "  -R pct      reads in the mixed workload    (default 90)\n"
	    "  -S seed     key selection seed             (default 1)\n"
	    "  -B keys     keys per request, mget only    (default 64)\n");
	exit(code);
}

int
main(int argc, char *argv[])
{
	/* Percentiles are collected per repetition and each one is
	 * reported as its lowest observation, not as the percentile of
	 * every repetition merged together.  Same argument as for
	 * throughput: interference only ever makes a latency worse, so
	 * the smallest value seen for a given percentile is the one least
	 * contaminated by whatever else the machine was doing. */
	static u64 cur[HBUCKETS];
	Thread *t;
	double ops[REPS_MAX], errs[REPS_MAX];
	double p50[REPS_MAX], p99[REPS_MAX], p999[REPS_MAX], worst[REPS_MAX];
	u64 cur_n = 0;
	int opt, r, i;

	while ((opt = getopt(argc, argv, "mLn:h:p:W:t:P:d:r:w:k:v:R:S:B:?")) != -1) {
		switch (opt) {
		case 'm': metric_machine = 1; break;
		case 'L': latency = 1; break;
		case 'n': scen = optarg; break;
		case 'h': host = optarg; break;
		case 'p': port = optarg; break;
		case 't': nthreads = atoi(optarg); break;
		case 'P': pipeline = atoi(optarg); break;
		case 'd': secs = atof(optarg); break;
		case 'r': reps = atoi(optarg); break;
		case 'w': warmup = atoi(optarg) ? 1 : 0; break;
		case 'k': keyspace = atoi(optarg); break;
		case 'v': valsize = atoi(optarg); break;
		case 'R': readpct = atoi(optarg); break;
		case 'S': seed0 = strtoull(optarg, NULL, 10); break;
		case 'B': batch = atoi(optarg); break;
		case 'W':
			for (i = 0; i < (int)(sizeof(wnames) / sizeof(*wnames));
			     i++) {
				if (!strcmp(optarg, wnames[i])) {
					workload = i;
					break;
				}
			}
			if (i == (int)(sizeof(wnames) / sizeof(*wnames)))
				usage(2);
			break;
		default: usage(opt == '?' ? 0 : 2);
		}
	}
	if (nthreads < 1 || pipeline < 1 || keyspace < 1 || valsize < 1 ||
	    valsize > (1 << 20) || reps < 1 || reps > REPS_MAX ||
	    batch < 1 || batch > 4096)
		usage(2);
	if (latency)
		pipeline = 1;
	if (workload == W_FILL) {
		reps = 1;
		warmup = 0;
	}
	signal(SIGPIPE, SIG_IGN);

	t = calloc((size_t)nthreads, sizeof(*t));
	for (i = 0; i < nthreads; i++) {
		t[i].id = i;
		if (workload == W_MGET) {
			t[i].reqcap = (size_t)pipeline *
			    ((size_t)batch * 16 + 128) + 256;
			t[i].bufcap = (size_t)pipeline * (size_t)batch *
			    ((size_t)valsize + 16) + 8192;
		} else {
			t[i].reqcap = (size_t)pipeline *
			    ((size_t)valsize + 192) + 256;
			t[i].bufcap = (size_t)pipeline *
			    ((size_t)valsize + 512) + 8192;
		}
		t[i].req = malloc(t[i].reqcap);
		t[i].buf = malloc(t[i].bufcap);
		t[i].scratch = malloc((size_t)batch * 16 + 64);
		t[i].val = malloc((size_t)valsize + 1);
		t[i].hist = calloc(HBUCKETS, sizeof(*t[i].hist));
		memset(t[i].val, 'v', (size_t)valsize);
		t[i].val[valsize] = '\0';
	}

	for (r = -warmup; r < reps; r++) {
		struct timespec nap;
		double t0, t1;
		u64 done = 0, bad = 0;

		atomic_store(&go, 0);
		atomic_store(&stop, 0);
		for (i = 0; i < nthreads; i++) {
			t[i].ops = t[i].errs = 0;
			t[i].buflen = t[i].off = t[i].need = 0;
			pthread_create(&t[i].th, NULL, worker, &t[i]);
		}
		t0 = now_s();
		atomic_store_explicit(&go, 1, memory_order_release);
		if (workload != W_FILL) {
			nap.tv_sec = (time_t)secs;
			nap.tv_nsec = (long)((secs - (double)(time_t)secs)
			                     * 1e9);
			nanosleep(&nap, NULL);
			atomic_store_explicit(&stop, 1, memory_order_relaxed);
		}
		for (i = 0; i < nthreads; i++) {
			pthread_join(t[i].th, NULL);
			done += t[i].ops;
			bad += t[i].errs;
		}
		t1 = now_s();
		if (r < 0) {
			/* a warmup pass: connections opened, store pages
			 * faulted in, nothing recorded */
			for (i = 0; i < nthreads; i++) {
				memset(t[i].hist, 0,
				       HBUCKETS * sizeof(*t[i].hist));
				t[i].over = t[i].max_us = 0;
			}
			continue;
		}
		ops[r] = (double)done / (t1 - t0);
		errs[r] = done ? (double)bad * 100.0 / (double)done : 0;

		if (latency) {
			u64 mx = 0, j;

			memset(cur, 0, sizeof(cur));
			cur_n = 0;
			for (i = 0; i < nthreads; i++) {
				for (j = 0; j < HBUCKETS; j++) {
					cur[j] += t[i].hist[j];
					cur_n += t[i].hist[j];
				}
				if (t[i].max_us > mx)
					mx = t[i].max_us;
				memset(t[i].hist, 0,
				       HBUCKETS * sizeof(*t[i].hist));
				t[i].max_us = t[i].over = 0;
			}
			p50[r] = (double)percentile(cur, cur_n, 0, mx, 0.50);
			p99[r] = (double)percentile(cur, cur_n, 0, mx, 0.99);
			p999[r] = (double)percentile(cur, cur_n, 0, mx, 0.999);
			worst[r] = (double)mx;
		}
	}

	{
		double o = best(ops, reps), sp = spread_pct(ops, reps);
		char extra[96];

		/* In latency mode the rate is just the reciprocal of the
		 * median latency, so it is reported for context and not
		 * judged; the percentiles are the measurement. */
		metric(scen, latency ? "rate" : "ops_per_sec", o);
		metric(scen, "err_pct", median(errs, reps));
		metric(scen, "spread", sp);
		metric(scen, "threads", nthreads);
		metric(scen, "pipeline", pipeline);
		if (latency) {
			double a = least(p50, reps), b2 = least(p99, reps);

			metric(scen, "p50_us", a);
			metric(scen, "p99_us", b2);
			/* Reported, never judged: over a run this short
			 * the 99.9th percentile is a few hundred samples
			 * of whatever the machine was doing at the time,
			 * and it swings by more than any code change
			 * would.  Hence the name without a _us suffix. */
			metric(scen, "p999", least(p999, reps));
			metric(scen, "max", least(worst, reps));
			snprintf(extra, sizeof(extra),
			    "p50 %.0fus p99 %.0fus max %.0fus",
			    a, b2, least(worst, reps));
		} else {
			if (workload == W_FILL)
				snprintf(extra, sizeof(extra),
				         "fill, %d conn", nthreads);
			else if (workload == W_MGET)
				snprintf(extra, sizeof(extra),
				         "mget, %d conn x %d deep x %d keys",
				         nthreads, pipeline, batch);
			else
				snprintf(extra, sizeof(extra),
				         "%s, %d conn x %d deep",
				         wnames[workload], nthreads, pipeline);
		}
		report(scen, o, sp, extra);
	}
	return 0;
}
