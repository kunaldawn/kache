/* kache-micro - benchmarks for the storage engine itself
 *
 * The HTTP benchmark is bounded by syscalls long before it is bounded by
 * the store, so a change to the hash, the allocator or the eviction path
 * barely moves it.  This one links the engine directly and calls it in a
 * loop, which is where those changes are visible.
 *
 * Numbers here are not comparable to the HTTP ones and are not meant to
 * be: they answer "did the engine get faster", not "how fast is the
 * server". */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "metric.h"
#include "store/db.h"
#include "util/clk.h"
#include "util/hash.h"
#include "util/util.h"

#define BATCH   64          /* ops between stop flag checks */
#define VAL_MAX 8192
#define KEY_MAX 32
#define FIELDS  32          /* fields per map in the container workloads */
#define QDEPTH  1024        /* entries a queue is held at */

static Db db;
static const char *path = "/tmp/kache-micro.db";
static u64 store_size = 64ull << 20;
static u32 keyspace = 100000;
static u32 valsize = 64;
static double secs = 0.5;
static int reps = 3;
static int warmup = 1;
static int maxthreads;

static u32 nmaps = 1, nqueues = 256;

static _Atomic int go, stop;
static _Atomic u64 newkey;    /* handed out by the insert workloads */

typedef struct Job {
	pthread_t th;
	int  tid;
	u64  ops;
	u64  misses;
	u64  errors;
} Job;

/* ---- helpers --------------------------------------------------------- */

static u32
mkkey(char *b, u64 i)
{
	b[0] = 'k';
	return 1 + (u32)fmt_u64(b + 1, i);
}

static u32
mkfld(char *b, u64 i)
{
	b[0] = 'f';
	return 1 + (u32)fmt_u64(b + 1, i);
}

static inline int
running(void)
{
	return !atomic_load_explicit(&stop, memory_order_relaxed);
}

static void
wait_go(void)
{
	while (!atomic_load_explicit(&go, memory_order_acquire))
		cpu_relax();
}

static u64
seed_of(const Job *j)
{
	return 0x9e3779b97f4a7c15ull * (u64)(j->tid + 1) + 12345;
}

static void
fill_keys(void)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	u64 i;

	memset(val, 'v', valsize);
	for (i = 0; i < keyspace; i++)
		db_set(&db, k, mkkey(k, i), val, valsize, DB_FOREVER, 0,
		       SET_ANY, 0, NULL);
}

/* Insert until the arena is nine tenths full, so the workloads that
 * follow measure the steady state rather than the easy first pass.
 *
 * The key count is watched as well: if the index saturates before the
 * arena does, inserting more only evicts, and waiting for a byte count
 * that will never arrive would hang.  That it can happen at all is a
 * property of the geometry, so the caller is told about it. */
static double
fill_arena(void)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbStats st;
	u64 i = 1000000, prev = 0;
	int n;

	memset(val, 'v', valsize);
	for (;;) {
		for (n = 0; n < 20000; n++, i++)
			db_set(&db, k, mkkey(k, i), val, valsize, DB_FOREVER,
			       0, SET_ANY, 0, NULL);
		db_stats(&db, &st);
		if (st.bytes * 10 >= st.capacity * 9 || st.keys <= prev)
			break;
		prev = st.keys;
	}
	return st.capacity ? (double)st.bytes * 100.0 / (double)st.capacity : 0;
}

/* ---- workloads ------------------------------------------------------- */

static void
w_hash16(Job *j)
{
	u8 buf[256];
	u64 acc = 0;
	int i;

	memset(buf, 'x', sizeof(buf));
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++)
			acc += hash_bytes(buf, 16, acc | 1);
		j->ops += BATCH;
	}
	j->errors += acc & 1;   /* a sink, so the loop is not elided */
}

static void
w_hash64(Job *j)
{
	u8 buf[256];
	u64 acc = 0;
	int i;

	memset(buf, 'x', sizeof(buf));
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++)
			acc += hash_bytes(buf, 64, acc | 1);
		j->ops += BATCH;
	}
	j->errors += acc & 1;
}

static void
w_get_hit(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbMeta m;
	u64 s = seed_of(j);
	int i;

	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u32 kl = mkkey(k, rng_next(&s) % keyspace);

			if (db_get(&db, k, kl, val, sizeof(val), &m) != DB_OK)
				j->misses++;
		}
		j->ops += BATCH;
	}
}

static void
w_get_miss(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbMeta m;
	u64 s = seed_of(j);
	int i;

	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u32 kl = mkkey(k, 9000000 + rng_next(&s) % keyspace);

			if (db_get(&db, k, kl, val, sizeof(val), &m) == DB_OK)
				j->errors++;
		}
		j->ops += BATCH;
	}
}

static void
w_set_over(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	u64 s = seed_of(j);
	int i;

	memset(val, 'v', valsize);
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u32 kl = mkkey(k, rng_next(&s) % keyspace);

			if (db_set(&db, k, kl, val, valsize, DB_FOREVER, 0,
			           SET_ANY, 0, NULL) != DB_OK)
				j->errors++;
		}
		j->ops += BATCH;
	}
}

static void
w_set_new(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	int i;

	memset(val, 'v', valsize);
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u64 id = atomic_fetch_add_explicit(&newkey, 1,
			             memory_order_relaxed);
			u32 kl = mkkey(k, 2000000 + id);

			if (db_set(&db, k, kl, val, valsize, DB_FOREVER, 0,
			           SET_ANY, 0, NULL) != DB_OK)
				j->errors++;
		}
		j->ops += BATCH;
	}
}

static void
w_incr(Job *j)
{
	char k[KEY_MAX];
	DbMeta m;
	i64 out;
	u64 s = seed_of(j);
	int i;

	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u32 kl = mkkey(k, rng_next(&s) % keyspace);

			if (db_incr(&db, k, kl, 1, 0, DB_FOREVER, 0, &out,
			            &m) != DB_OK)
				j->errors++;
		}
		j->ops += BATCH;
	}
}

static void
w_del_ins(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	u64 s = seed_of(j);
	int i;

	memset(val, 'v', valsize);
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i += 2) {
			u32 kl = mkkey(k, rng_next(&s) % keyspace);

			db_del(&db, k, kl, 0, 0);
			db_set(&db, k, kl, val, valsize, DB_FOREVER, 0,
			       SET_ANY, 0, NULL);
		}
		j->ops += BATCH;
	}
}

static void
w_mixed(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbMeta m;
	u64 s = seed_of(j);
	int i;

	memset(val, 'v', valsize);
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u64 r = rng_next(&s);
			u32 kl = mkkey(k, (r >> 8) % keyspace);

			if ((r & 0xff) < 26)
				db_set(&db, k, kl, val, valsize, DB_FOREVER, 0,
				       SET_ANY, 0, NULL);
			else if (db_get(&db, k, kl, val, sizeof(val),
			                &m) != DB_OK)
				j->misses++;
		}
		j->ops += BATCH;
	}
}

/* ---- containers -------------------------------------------------------
 *
 * The same total number of items as the plain workloads, arranged as
 * keyspace/FIELDS maps of FIELDS fields each, so the two are comparable:
 * what changes is the shape of the lookup, not how much is stored. */

static void
kkv_defaults(DbKkvOpt *o)
{
	memset(o, 0, sizeof(*o));
	o->ttl = DB_FOREVER;
	o->kttl = DB_FOREVER;
	o->mode = SET_ANY;
}

static void
q_defaults(DbQOpt *o, int right)
{
	memset(o, 0, sizeof(*o));
	o->ttl = DB_FOREVER;
	o->qttl = DB_FOREVER;
	o->right = right;
}

static void
fill_maps(void)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX], f[KEY_MAX];
	DbKkvOpt o;
	u64 i;
	u32 j;

	memset(val, 'v', valsize);
	kkv_defaults(&o);
	nmaps = keyspace / FIELDS;
	if (!nmaps)
		nmaps = 1;
	for (i = 0; i < nmaps; i++)
		for (j = 0; j < FIELDS; j++)
			db_kkv_set(&db, k, mkkey(k, i), f, mkfld(f, j),
			           val, valsize, &o, NULL, NULL);
}

static void
fill_queues(void)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbQOpt o;
	DbItem it;
	u64 i;
	u32 j, got;

	memset(val, 'v', valsize);
	q_defaults(&o, Q_RIGHT);
	it.k = NULL;
	it.kl = 0;
	it.v = val;
	it.vl = valsize;
	it.ttl = DB_FOREVER;
	nqueues = keyspace / QDEPTH;
	if (nqueues < 16)
		nqueues = 16;
	for (i = 0; i < nqueues; i++)
		for (j = 0; j < QDEPTH; j++)
			db_q_push(&db, k, mkkey(k, i), &it, 1, &o, &got, NULL);
}

static int
sink_fld(void *arg, const void *f, u32 fl, const void *v, u32 vl,
         const DbMeta *m)
{
	u64 *n = arg;

	(void)f; (void)fl; (void)v; (void)vl; (void)m;
	(*n)++;
	return 0;
}

static int
sink_ent(void *arg, const void *v, u32 vl, const DbQMeta *e)
{
	u64 *n = arg;

	(void)v; (void)vl; (void)e;
	(*n)++;
	return 0;
}

static void
w_kkv_get(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX], f[KEY_MAX];
	DbMeta m;
	u64 s = seed_of(j);
	int i;

	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u64 r = rng_next(&s);
			u32 kl = mkkey(k, (r >> 8) % nmaps);
			u32 fl = mkfld(f, (r >> 40) % FIELDS);

			if (db_kkv_get(&db, k, kl, f, fl, val, sizeof(val),
			               &m) != DB_OK)
				j->misses++;
		}
		j->ops += BATCH;
	}
}

static void
w_kkv_set(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX], f[KEY_MAX];
	DbKkvOpt o;
	u64 s = seed_of(j);
	int i;

	memset(val, 'v', valsize);
	kkv_defaults(&o);
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u64 r = rng_next(&s);
			u32 kl = mkkey(k, (r >> 8) % nmaps);
			u32 fl = mkfld(f, (r >> 40) % FIELDS);

			if (db_kkv_set(&db, k, kl, f, fl, val, valsize, &o,
			               NULL, NULL) != DB_OK)
				j->errors++;
		}
		j->ops += BATCH;
	}
}

/* one request, every field of one map: what the shape is actually for */
static void
w_kkv_scan(Job *j)
{
	char k[KEY_MAX];
	u64 s = seed_of(j), seen = 0;
	int i;

	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u32 kl = mkkey(k, rng_next(&s) % nmaps);

			if (db_kkv_scan(&db, k, kl, sink_fld, &seen, NULL)
			    != DB_OK)
				j->misses++;
		}
		j->ops += BATCH * FIELDS;
	}
	j->errors += seen & 0;
}

/* Build a map and throw it away.  The interesting part is that the
 * throwing away is constant time: the sweeper takes it apart later. */
static void
w_kkv_churn(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX], f[KEY_MAX];
	DbKkvOpt o;
	u64 s = seed_of(j);
	int i;

	memset(val, 'v', valsize);
	kkv_defaults(&o);
	wait_go();
	while (running()) {
		u32 kl = mkkey(k, rng_next(&s));

		for (i = 0; i < FIELDS; i++)
			if (db_kkv_set(&db, k, kl, f, mkfld(f, (u64)i), val,
			               valsize, &o, NULL, NULL) != DB_OK)
				j->errors++;
		db_kkv_drop(&db, k, kl);
		j->ops += FIELDS + 1;
	}
}

static void
w_q_cycle(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbQOpt push, pop;
	DbItem it;
	u64 s = seed_of(j), seen = 0;
	u32 got;
	int i;

	memset(val, 'v', valsize);
	q_defaults(&push, Q_RIGHT);
	q_defaults(&pop, Q_LEFT);
	it.k = NULL;
	it.kl = 0;
	it.v = val;
	it.vl = valsize;
	it.ttl = DB_FOREVER;
	wait_go();
	while (running()) {
		for (i = 0; i < BATCH; i++) {
			u32 kl = mkkey(k, rng_next(&s) % nqueues);

			if (db_q_push(&db, k, kl, &it, 1, &push, &got, NULL)
			    != DB_OK)
				j->errors++;
			if (db_q_pop(&db, k, kl, 1, &pop, sink_ent, &seen,
			             &got, NULL) != DB_OK)
				j->misses++;
		}
		j->ops += BATCH * 2;
	}
}

/* one pop of many entries, which is the only way to beat the round trip */
static void
w_q_drain(Job *j)
{
	u8 val[VAL_MAX];
	char k[KEY_MAX];
	DbQOpt push, pop;
	DbItem it[64];
	u64 s = seed_of(j), seen = 0;
	u32 got, i;

	memset(val, 'v', valsize);
	q_defaults(&push, Q_RIGHT);
	q_defaults(&pop, Q_LEFT);
	for (i = 0; i < LEN(it); i++) {
		it[i].k = NULL;
		it[i].kl = 0;
		it[i].v = val;
		it[i].vl = valsize;
		it[i].ttl = DB_FOREVER;
	}
	wait_go();
	while (running()) {
		u32 kl = mkkey(k, rng_next(&s) % nqueues);

		if (db_q_push(&db, k, kl, it, (u32)LEN(it), &push, &got, NULL)
		    != DB_OK)
			j->errors++;
		if (db_q_pop(&db, k, kl, (u32)LEN(it), &pop, sink_ent, &seen,
		             &got, NULL) != DB_OK)
			j->misses++;
		j->ops += LEN(it) * 2;
	}
}

/* ---- scenario driver -------------------------------------------------- */

enum { PF_NONE, PF_KEYS, PF_FULL, PF_MAPS, PF_QUEUES };

typedef struct Scen {
	const char *name;
	void      (*fn)(Job *);
	int         threads;     /* 0 means one per cpu */
	int         prefill;
	int         reset;       /* empty the store first */
} Scen;

static void *
thread_main(void *arg)
{
	Job *j = arg;
	extern void (*cur_fn)(Job *);

	cur_fn(j);
	return NULL;
}

void (*cur_fn)(Job *);

static void
run_scen(const Scen *s)
{
	double ops[REPS_MAX], misses[REPS_MAX];
	int nt = s->threads ? s->threads : maxthreads;
	double filled = 0;
	int r, i;

	if (nt > 256)
		nt = 256;
	for (r = -warmup; r < reps; r++) {
		Job job[256];
		struct timespec nap;
		double t0, t1;
		u64 total = 0, miss = 0;

		if (s->reset)
			db_flush(&db);
		if (s->prefill == PF_KEYS)
			fill_keys();
		else if (s->prefill == PF_MAPS)
			fill_maps();
		else if (s->prefill == PF_QUEUES)
			fill_queues();
		else if (s->prefill == PF_FULL)
			filled = fill_arena();

		memset(job, 0, sizeof(job[0]) * (size_t)nt);
		atomic_store(&go, 0);
		atomic_store(&stop, 0);
		cur_fn = s->fn;
		for (i = 0; i < nt; i++) {
			job[i].tid = i;
			pthread_create(&job[i].th, NULL, thread_main, &job[i]);
		}
		t0 = now_s();
		atomic_store_explicit(&go, 1, memory_order_release);
		nap.tv_sec = (time_t)secs;
		nap.tv_nsec = (long)((secs - (double)(time_t)secs) * 1e9);
		nanosleep(&nap, NULL);
		atomic_store_explicit(&stop, 1, memory_order_relaxed);
		for (i = 0; i < nt; i++) {
			pthread_join(job[i].th, NULL);
			total += job[i].ops;
			miss += job[i].misses + job[i].errors;
		}
		t1 = now_s();
		if (r < 0)
			continue;         /* warmup, measured but discarded */
		ops[r] = (double)total / (t1 - t0);
		misses[r] = total ? (double)miss * 100.0 / (double)total : 0;
	}

	{
		double o = best(ops, reps), sp = spread_pct(ops, reps);
		char extra[64];

		metric(s->name, "ops_per_sec", o);
		metric(s->name, "miss_pct", median(misses, reps));
		metric(s->name, "spread", sp);
		metric(s->name, "threads", nt);
		if (s->prefill == PF_FULL)
			metric(s->name, "arena_fill", filled);
		if (s->prefill == PF_FULL)
			snprintf(extra, sizeof(extra), "%d thread%s, arena %.0f%% full",
			         nt, nt == 1 ? "" : "s", filled);
		else
			snprintf(extra, sizeof(extra), "%d thread%s", nt,
			         nt == 1 ? "" : "s");
		report(s->name, o, sp, extra);
	}
}

static const Scen scens[] = {
	{ "micro_hash16",    w_hash16,  1, PF_NONE, 0 },
	{ "micro_hash64",    w_hash64,  1, PF_NONE, 0 },
	{ "micro_get_hit",   w_get_hit, 1, PF_KEYS, 1 },
	{ "micro_get_miss",  w_get_miss, 1, PF_KEYS, 1 },
	{ "micro_set_over",  w_set_over, 1, PF_KEYS, 1 },
	{ "micro_set_new",   w_set_new, 1, PF_NONE, 1 },
	{ "micro_incr",      w_incr,    1, PF_NONE, 1 },
	{ "micro_del_ins",   w_del_ins, 1, PF_KEYS, 1 },
	{ "micro_evict",     w_set_new, 1, PF_FULL, 1 },
	{ "micro_mixed_t1",  w_mixed,   1, PF_KEYS, 1 },
	{ "micro_get_mt",    w_get_hit, 0, PF_KEYS, 1 },
	{ "micro_set_mt",    w_set_over, 0, PF_KEYS, 1 },
	{ "micro_mixed_mt",  w_mixed,   0, PF_KEYS, 1 },
	{ "micro_evict_mt",  w_set_new, 0, PF_FULL, 1 },
	{ "micro_kkv_get",   w_kkv_get, 1, PF_MAPS, 1 },
	{ "micro_kkv_set",   w_kkv_set, 1, PF_MAPS, 1 },
	{ "micro_kkv_scan",  w_kkv_scan, 1, PF_MAPS, 1 },
	{ "micro_kkv_churn", w_kkv_churn, 1, PF_NONE, 1 },
	{ "micro_q_cycle",   w_q_cycle, 1, PF_QUEUES, 1 },
	{ "micro_q_drain",   w_q_drain, 1, PF_QUEUES, 1 },
	{ "micro_kkv_get_mt", w_kkv_get, 0, PF_MAPS, 1 },
	{ "micro_kkv_set_mt", w_kkv_set, 0, PF_MAPS, 1 },
	{ "micro_q_cycle_mt", w_q_cycle, 0, PF_QUEUES, 1 }
};

static void *
ticker(void *arg)
{
	struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

	(void)arg;
	for (;;) {
		nanosleep(&nap, NULL);
		clk_update();
	}
	return NULL;
}

static void
usage(int code)
{
	fputs("usage: kache-micro [-m] [-f file] [-s size] [-k keys] "
	      "[-v bytes]\n"
	      "                   [-d seconds] [-r reps] [-t threads] "
	      "[-o scenario]\n"
	      "  -m          emit result lines instead of a table\n"
	      "  -f file     store used for the run (recreated)\n"
	      "  -s size     its size                    (default 64M)\n"
	      "  -k keys     keyspace                    (default 100000)\n"
	      "  -v bytes    value size                  (default 64)\n"
	      "  -d seconds  measured time per repetition (default 0.5)\n"
	      "  -r reps     repetitions, best reported   (default 3)\n"
	      "  -w 0|1      discard one warmup pass first (default 1)\n"
	      "  -t threads  threads for the _mt scenarios (default ncpu)\n"
	      "  -o scen     run only scenarios containing this substring\n",
	      code ? stderr : stdout);
	exit(code);
}

int
main(int argc, char *argv[])
{
	MapCfg mc;
	pthread_t tick;
	const char *only = NULL;
	size_t i;
	int opt;

	maxthreads = ncpu();
	while ((opt = getopt(argc, argv, "mf:s:k:v:d:r:t:o:w:h")) != -1) {
		switch (opt) {
		case 'm': metric_machine = 1; break;
		case 'f': path = optarg; break;
		case 's':
			if (parse_size(optarg, &store_size) < 0)
				usage(2);
			break;
		case 'k': keyspace = (u32)atoi(optarg); break;
		case 'v': valsize = (u32)atoi(optarg); break;
		case 'd': secs = atof(optarg); break;
		case 'r': reps = atoi(optarg); break;
		case 'w': warmup = atoi(optarg) ? 1 : 0; break;
		case 't': maxthreads = atoi(optarg); break;
		case 'o': only = optarg; break;
		default:  usage(opt == 'h' ? 0 : 2);
		}
	}
	if (valsize > VAL_MAX || !valsize || !keyspace)
		usage(2);
	if (reps < 1 || reps > REPS_MAX)
		usage(2);
	if (maxthreads < 1)
		maxthreads = 1;

	verbosity(0);
	clk_init();
	pthread_create(&tick, NULL, ticker, NULL);
	pthread_detach(tick);

	memset(&mc, 0, sizeof(mc));
	mc.path = path;
	mc.size = store_size;
	mc.maxkey = CFG_MAX_KEY;
	mc.maxval = CFG_MAX_VAL;
	mc.avg_item = CFG_AVG_ITEM;
	mc.flags = KM_FRESH;
	if (db_open(&db, &mc) < 0)
		return 1;

	if (!metric_machine)
		printf("engine, %llu MiB store, %u keys of %u bytes, "
		       "%.2gs x %d\n", (unsigned long long)(store_size >> 20),
		       keyspace, valsize, secs, reps);
	for (i = 0; i < LEN(scens); i++) {
		if (only && !strstr(scens[i].name, only))
			continue;
		run_scen(&scens[i]);
	}
	db_close(&db);
	unlink(path);
	return 0;
}
