/* kache - the HTTP front end
 *
 * One epoll loop per thread, each with its own listening socket opened
 * with SO_REUSEPORT so the kernel spreads new connections itself.  No
 * accept lock, no handoff queue, no shared connection state. */
#ifndef KACHE_SERVER_H
#define KACHE_SERVER_H

#include "store/db.h"
#include "util/util.h"

typedef struct ServerCfg {
	const char *addr;
	const char *port;
	unsigned    threads;
	unsigned    conns;      /* per thread */
	int         backlog;
	u64         idle_ms;
	u64         sync_ms;
	i64         default_ttl;
	int         allow_flush;
} ServerCfg;

int server_run(Db *db, const ServerCfg *cfg);

#endif /* KACHE_SERVER_H */
