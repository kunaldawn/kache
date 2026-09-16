/* kache - request to store operation mapping */
#ifndef KACHE_ROUTE_H
#define KACHE_ROUTE_H

#include "http/buf.h"
#include "http/http.h"
#include "http/stats.h"
#include "store/db.h"

typedef struct Ctx {
	Db    *db;
	Stats *st;
	i64    default_ttl;   /* ms, DB_FOREVER when items never expire */
	size_t max_req;       /* largest request we will buffer */
	u64    started;       /* ms since the epoch */
	int    allow_flush;
	int    minimal;       /* omit the headers a cache client ignores */
	int    minimal_req;   /* minimal, unless this client needs them all;
	                       * set by route(), read by the handlers */
	int    head;          /* this request is a HEAD, so no body goes out */
} Ctx;

/* appends one complete response to out */
void route(Ctx *c, const Req *r, Buf *out, int *keepalive);

#endif /* KACHE_ROUTE_H */
