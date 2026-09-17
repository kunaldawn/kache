/* kache - a multi threaded, file backed key/value cache.
 *
 * This file is only the command line: it turns arguments into a store
 * geometry and a server configuration, opens the store and runs the
 * server until a signal arrives.  See README.md for the design. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "cluster/cluster.h"
#include "http/server.h"
#include "store/db.h"
#include "util/clk.h"
#include "util/hash.h"
#include "util/util.h"

static const char usage_text[] =
"usage: kache [-mPHnMAFqvh] [-l addr] [-p port] [-f file] [-s size]\n"
"             [-S shards] [-t threads] [-c conns] [-e ttl] [-y ms]\n"
"             [-i idle] [-b backlog] [-B bytes] [-K bytes] [-V bytes]\n"
"             [-X ms] [-C nodes] [-N index] [-Y ms] [-U addrs]\n"
"\n"
"  -l addr    address to listen on          (default " CFG_ADDR ")\n"
"  -p port    port to listen on             (default " CFG_PORT ")\n"
"  -f file    backing file                  (default " CFG_PATH ")\n"
"  -s size    backing file size             (default 1G)\n"
"  -S n       shards, 0 picks one           (default 0)\n"
"  -t n       worker threads, 0 = one/cpu   (default 0)\n"
"  -c n       connections per thread        (default 8192)\n"
"  -e sec     default ttl, 0 = never        (default 0)\n"
"  -y ms      msync interval, 0 = off       (default 1000)\n"
"  -i sec     idle connection timeout       (default 60)\n"
"  -b n       listen backlog                (default 1024)\n"
"  -B bytes   expected average item size    (default 128)\n"
"  -K bytes   maximum key size              (default 255)\n"
"  -V bytes   maximum value size            (default 256k)\n"
"  -m         lock the store into memory\n"
"  -P         fault the whole file in at startup\n"
"  -H         ask for transparent huge pages\n"
"  -n         start from an empty store\n"
"  -M         drop optional response headers for speed\n"
"  -X ms      serve hot keys from a per worker set, 0 = off\n"
"  -C nodes   cluster members, host:port,host:port,... in order\n"
"  -N index   this node's position in that list      (default 0)\n"
"  -Y ms      replication flush interval            (default 50)\n"
"  -U addrs   addresses clients should use, same order as -C\n"
"  -A         pin each worker to one cpu\n"
"  -F         enable POST /flush\n"
"  -q         quiet\n"
"  -v         print version and exit\n"
"  -h         this message\n"
"\n"
"Geometry (size, shards, key and value limits) is fixed when the file is\n"
"created; reopening an existing store keeps it.  Use -n to change it.\n"
"\n"
"With -C every node keeps a copy of every key, so a read is answered\n"
"locally by whichever node it reaches.  A key's writes belong to one\n"
"owner, fixed by the hash, and a write that arrives elsewhere is answered\n"
"with a 307 naming the owner.  Every node must be given the same list in\n"
"the same order, and the stores must share a hash seed: build the cluster\n"
"from one node's file, or create them all with -n from the same -s and\n"
"-S.  A write is visible on other nodes after one -Y interval.\n"
"\n"
"-C is the address nodes use to reach each other.  Where that is not the\n"
"address a client can reach - behind NAT, in a container, under a proxy -\n"
"-U gives the client facing address of each node in the same order, and\n"
"redirects name that instead.\n";

static void
usage(int code)
{
	fputs(usage_text, code ? stderr : stdout);
	exit(code);
}

static u64
must_size(const char *s, const char *what)
{
	u64 v;

	if (parse_size(s, &v) < 0)
		die("%s: not a size: %s", what, s);
	return v;
}

static u64
must_num(const char *s, const char *what)
{
	u64 v;

	if (parse_u64(s, strlen(s), &v) < 0)
		die("%s: not a number: %s", what, s);
	return v;
}

int
main(int argc, char *argv[])
{
	MapCfg mc;
	ServerCfg sc;
	Db db;
	ClusterCfg cc;
	Cluster *cl = NULL;
	u64 ttl_sec = 0, idle_sec = CFG_IDLE_MS / 1000;
	int opt;

	memset(&mc, 0, sizeof(mc));
	mc.path = CFG_PATH;
	mc.size = CFG_SIZE;
	mc.shards = CFG_SHARDS;
	mc.maxkey = CFG_MAX_KEY;
	mc.maxval = CFG_MAX_VAL;
	mc.avg_item = CFG_AVG_ITEM;

	memset(&cc, 0, sizeof(cc));
	cc.flush_ms = CFG_REPL_MS;

	memset(&sc, 0, sizeof(sc));
	sc.addr = CFG_ADDR;
	sc.port = CFG_PORT;
	sc.threads = CFG_THREADS;
	sc.conns = CFG_CONNS;
	sc.backlog = CFG_BACKLOG;
	sc.sync_ms = CFG_SYNC_MS;
	sc.default_ttl = CFG_TTL_MS ? CFG_TTL_MS : DB_FOREVER;
	sc.minimal = CFG_MINIMAL;
	sc.affinity = CFG_AFFINITY;
	sc.hot_ms = CFG_HOT_MS;

	while ((opt = getopt(argc, argv, "l:p:f:s:S:t:c:e:y:i:b:B:K:V:X:C:N:Y:U:mPHnMAFqvh")) != -1) {
		switch (opt) {
		case 'l': sc.addr = optarg; break;
		case 'p': sc.port = optarg; break;
		case 'f': mc.path = optarg; break;
		case 's': mc.size = must_size(optarg, "-s"); break;
		case 'S': mc.shards = (u32)must_num(optarg, "-S"); break;
		case 't': sc.threads = (unsigned)must_num(optarg, "-t"); break;
		case 'c': sc.conns = (unsigned)must_num(optarg, "-c"); break;
		case 'e': ttl_sec = must_num(optarg, "-e"); break;
		case 'y': sc.sync_ms = must_num(optarg, "-y"); break;
		case 'i': idle_sec = must_num(optarg, "-i"); break;
		case 'b': sc.backlog = (int)must_num(optarg, "-b"); break;
		case 'B': mc.avg_item = (u32)must_size(optarg, "-B"); break;
		case 'K': mc.maxkey = (u32)must_size(optarg, "-K"); break;
		case 'V': mc.maxval = (u32)must_size(optarg, "-V"); break;
		case 'm': mc.flags |= KM_LOCKED; break;
		case 'P': mc.flags |= KM_PREFAULT; break;
		case 'H': mc.flags |= KM_HUGE; break;
		case 'n': mc.flags |= KM_FRESH; break;
		case 'M': sc.minimal = 1; break;
		case 'X': sc.hot_ms = must_num(optarg, "-X"); break;
		case 'C': cc.peers = optarg; break;
		case 'N': cc.self = (unsigned)must_num(optarg, "-N"); break;
		case 'Y': cc.flush_ms = must_num(optarg, "-Y"); break;
		case 'U': cc.advertise = optarg; break;
		case 'A': sc.affinity = 1; break;
		case 'F': sc.allow_flush = 1; break;
		case 'q': verbosity(0); break;
		case 'v': puts("kache " VERSION); return 0;
		default:  usage(opt == 'h' ? 0 : 2);
		}
	}
	if (optind != argc)
		usage(2);

	if (mc.maxkey < 1 || mc.maxkey > CFG_MAX_KEY)
		die("-K must be between 1 and %u; raise CFG_MAX_KEY in "
		    "config.h to go higher", CFG_MAX_KEY);
	if (mc.maxval < 1 || mc.maxval > CFG_MAX_VAL)
		die("-V must be between 1 and %u; raise CFG_MAX_VAL in "
		    "config.h to go higher", CFG_MAX_VAL);
	if (!sc.conns)
		die("-c must be at least 1");
	sc.default_ttl = ttl_sec ? (i64)ttl_sec * 1000 : DB_FOREVER;
	sc.idle_ms = idle_sec * 1000;
	/* A connection's activity stamp is only refreshed every
	 * CFG_TOUCH_MS, so an idle timeout of the same order would close
	 * connections that are still in the middle of a request. */
	if (sc.idle_ms && sc.idle_ms <= (u64)CFG_TOUCH_MS * 4)
		die("-i must be more than %u seconds, or CFG_TOUCH_MS must be "
		    "lowered in config.h: an active connection's timestamp "
		    "may lag by %ums", (unsigned)(CFG_TOUCH_MS * 4 / 1000),
		    (unsigned)CFG_TOUCH_MS);

	/* Nodes work out who owns a key from the hash alone, so they have
	 * to hash alike, and a store seeds itself at random.  Deriving the
	 * seed from the member list means the cluster agrees without anyone
	 * configuring it: every node is already given the same -C, and a
	 * store created under a different one is refused by map_open rather
	 * than serving a keyspace its peers disagree about.  The list is
	 * also the thing that must not differ, so tying the two together
	 * turns a silent misconfiguration into a startup failure. */
	if (cc.peers) {
		mc.seed = hash_bytes(cc.peers, strlen(cc.peers),
		                     0x6b61636865ull);
		if (!mc.seed)
			mc.seed = 1;   /* 0 means "no seed asked for" */
	}
	if (cc.advertise && !cc.peers)
		die("-U names the client facing address of each -C node, so "
		    "it means nothing without -C");
	if (cc.peers && !cc.flush_ms)
		die("-Y must be at least 1ms: it is the coalescing window "
		    "that keeps peer traffic independent of the write rate");

	clk_init();
	if (db_open(&db, &mc) < 0)
		return 1;
	if (cl_open(&cl, &db, &cc) < 0) {
		db_close(&db);
		return 1;
	}
	sc.cl = cl;

	if (server_run(&db, &sc) < 0) {
		cl_close(cl);
		db_close(&db);
		return 1;
	}
	/* The flusher is stopped before the store is closed, and it sends
	 * whatever was still pending on its way out, so a clean shutdown
	 * does not strand a write on this node alone. */
	cl_close(cl);
	db_close(&db);
	info("stopped cleanly");
	return 0;
}
