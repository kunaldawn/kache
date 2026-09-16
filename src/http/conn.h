/* kache - one client connection
 *
 * Level triggered epoll with the interest set following the buffers: a
 * connection is readable-interested while it owes no output, and
 * writable-interested while it does.  That single rule gives pipelining
 * back pressure for free - a client that stops reading stops being read
 * from. */
#ifndef KACHE_CONN_H
#define KACHE_CONN_H

#include "http/buf.h"
#include "http/route.h"
#include "util/util.h"

typedef struct Conn {
	int   tag;            /* EV_CONN; epoll data is tagged by its first word */
	int   fd;
	int   epfd;
	u32   events;         /* currently registered with epoll */
	Buf   in;
	Buf   out;            /* out.off is how much of it has been sent */
	u64   atime;          /* ms of the last activity */
	struct Conn *prev, *next;   /* activity order, oldest first */
	struct Conn *fnext;         /* free list */
	u8    open;
	u8    closing;        /* finish writing, then hang up */
	u8    continued;      /* already answered Expect: 100-continue */
} Conn;

typedef struct Pool {
	Conn *slots;
	Conn *freelist;
	Conn *head, *tail;
	u32   cap;
	u32   used;
} Pool;

void  pool_init(Pool *p, u32 cap);
void  pool_fini(Pool *p);

Conn *conn_open(Pool *p, int fd, int epfd, u64 now);
void  conn_close(Pool *p, Conn *c, Stats *st);
void  conn_touch(Pool *p, Conn *c, u64 now);
/* returns -1 when the connection should be closed */
int   conn_event(Conn *c, Ctx *ctx, u32 events);
/* oldest connection, or NULL */
Conn *pool_oldest(Pool *p);

#endif /* KACHE_CONN_H */
