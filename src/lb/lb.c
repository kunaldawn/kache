/* kache-lb - a load balancer for a kache cluster.
 *
 * Why this exists, and why it is not an HTTP proxy.
 *
 * nginx in front of a three node cluster measured 1,984 ops/s against
 * 517,900 straight at a node - 261x - and almost none of that is nginx
 * being slow.  It is that nginx does not pipeline to an upstream: a
 * client sending sixteen requests in one segment has them replayed to
 * the backend one at a time, each waiting for the last to answer.  The
 * proxy destroys the single property kache's front end is built around.
 *
 * So this one never looks at the bytes.  It is an L4 connection
 * balancer: accept, pick a backend, and shuttle bytes in both directions
 * until one end hangs up.  A pipelined batch crosses it as a batch,
 * because it is just bytes in a buffer, and the balancer has no idea
 * where one request ends and the next begins.
 *
 * That is only viable because of how the cluster is built.  Every node
 * holds a full copy, so any node can answer any read and a connection
 * does not have to be steered per request.  Writes are not symmetric,
 * but they do not need this balancer either: a node that does not own
 * the key answers 307 naming the owner, and -U makes that an address the
 * client can reach, so the client goes straight there.  Between the two
 * there is nothing left for an L7 proxy to decide.
 *
 * Bytes are moved with read and write rather than splice.  splice avoids
 * the copy but pays pipe bookkeeping per transfer, and it is a losing
 * trade for small messages - HAProxy reaches the same conclusion and
 * only splices above a size threshold.  A cache's values are small, and
 * at pipeline depth sixteen one read already carries sixteen requests,
 * so the copy is a few dozen nanoseconds amortised over all of them.
 *
 * The shape is kache's own: one epoll loop per thread, each with its own
 * listening socket opened with SO_REUSEPORT, so there is no accept lock
 * and no shared connection state.  A connection and its backend belong
 * to one thread for their whole life, so nothing here takes a lock. */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "util/util.h"

#define LB_MAX_BACKENDS 64
#define LB_BUF          16384u   /* per direction, per connection */
#define LB_EVENTS       256

enum { EV_LISTEN = 0, EV_CLIENT, EV_BACKEND, EV_WAKE };

typedef struct Backend {
	char  addr[300];
	char  host[256];
	char  port[16];
	struct sockaddr_storage sa;
	socklen_t salen;
	int   family, socktype, protocol;
	_Atomic int up;
	_Atomic u64 opened, failed;
} Backend;

/* epoll hands back one pointer, and both of a pair's descriptors are in
 * the same epoll, so each side carries its own tag and a way back to the
 * pair it belongs to. */
typedef struct Conn Conn;

typedef struct Side {
	int   tag;
	Conn *conn;
} Side;

struct Conn {
	Side  cside, bside;       /* tags for epoll, never moved */
	int   cfd, bfd;
	u32   cev, bev;           /* what each fd is registered for */
	u8   *c2b, *b2c;
	u32   c2b_len, c2b_off;   /* live bytes are [off, len) */
	u32   b2c_len, b2c_off;
	u8    connecting;
	u8    ceof, beof;         /* that side sent us a FIN */
	/* What the kernel last said each side could do.  Without these the
	 * shuttle attempts all four transfers on every wakeup and most of
	 * them return EAGAIN, which is two or three syscalls spent per
	 * event to be told there is nothing to do.  Interest is level
	 * triggered, so clearing a flag on EAGAIN is safe: if the
	 * condition is still true epoll says so again. */
	u8    cr, cw, br, bw;
	u8    open;
	Conn *fnext;
};

typedef struct Worker {
	Side      listen_tag;
	Side      wake_tag;
	int       id;
	int       epfd, lfd, wfd;
	Conn     *slots;
	Conn     *freelist;
	u32       cap, used;
	pthread_t th;
	_Atomic u64 accepted, closed, refused;
} Worker;

static Backend backends[LB_MAX_BACKENDS];
static unsigned nbackends;
static Worker *workers;
static unsigned nworkers;
static _Atomic int stopping;
static _Atomic u64 rr;          /* round robin cursor, shared */
static unsigned health_ms = 1000;
static unsigned maxconn = 4096;

/* ---- backends -------------------------------------------------------- */

/* A backend's address is kept as a sockaddr rather than looked up per
 * connection, because a connect happens on the accept path and a
 * blocking DNS lookup has no business inside an event loop.  It is
 * re-resolved by the health thread while a backend is down: a container
 * that restarts usually comes back on a different address, and a
 * balancer that resolved once at startup would keep probing an address
 * nothing answers on and never notice the backend return. */
static int
resolve(Backend *b)
{
	struct addrinfo hints, *res;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(b->host, b->port, &hints, &res) != 0)
		return -1;
	memcpy(&b->sa, res->ai_addr, res->ai_addrlen);
	b->salen = res->ai_addrlen;
	b->family = res->ai_family;
	b->socktype = res->ai_socktype;
	b->protocol = res->ai_protocol;
	freeaddrinfo(res);
	return 0;
}

static int
add_backend(const char *spec)
{
	Backend *b = &backends[nbackends];
	const char *colon;
	size_t n;

	if (nbackends >= LB_MAX_BACKENDS)
		return -1;
	if (strlen(spec) >= sizeof(b->addr))
		return -1;
	snprintf(b->addr, sizeof(b->addr), "%s", spec);
	if (!(colon = strrchr(b->addr, ':'))) {
		warn("%s: no port", spec);
		return -1;
	}
	n = (size_t)(colon - b->addr);
	memcpy(b->host, b->addr, n);
	b->host[n] = '\0';
	snprintf(b->port, sizeof(b->port), "%s", colon + 1);

	if (resolve(b) < 0) {
		warn("%s: cannot resolve", spec);
		return -1;
	}
	atomic_store(&b->up, 1);   /* assumed up until a check says otherwise */
	nbackends++;
	return 0;
}

/* Round robin over the backends that are up.  One relaxed increment on a
 * shared counter, paid once per accepted connection rather than once per
 * request, so the sharing costs nothing measurable and the spread is
 * global rather than per thread - which matters, because SO_REUSEPORT
 * hands connections out by a hash of the four tuple and a small client
 * pool does not arrive evenly. */
static int
pick_backend(void)
{
	unsigned i, start;

	if (!nbackends)
		return -1;
	start = (unsigned)(atomic_fetch_add_explicit(&rr, 1,
	                   memory_order_relaxed) % nbackends);
	for (i = 0; i < nbackends; i++) {
		unsigned k = (start + i) % nbackends;

		/* Acquire pairs with the release in the health thread, so a
		 * backend seen as up is seen with the address that was
		 * resolved for it. */
		if (atomic_load_explicit(&backends[k].up, memory_order_acquire))
			return (int)k;
	}
	return -1;   /* everything is down; the caller answers accordingly */
}

/* ---- health checks ---------------------------------------------------
 *
 * One thread, blocking sockets, one backend at a time.  It runs once a
 * second against a handful of backends, so the simplest possible
 * implementation is the right one; putting it in the event loops would
 * complicate every loop to save nothing. */
static int
probe(Backend *b)
{
	static const char req[] = "GET /health HTTP/1.1\r\nHost: lb\r\n"
	                          "Connection: close\r\n\r\n";
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	char buf[128];
	ssize_t got;
	int fd, ok = 0;

	if ((fd = socket(b->family, b->socktype | SOCK_CLOEXEC,
	                 b->protocol)) < 0)
		return 0;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&b->sa, b->salen) == 0 &&
	    write(fd, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1) &&
	    (got = read(fd, buf, sizeof(buf) - 1)) > 12) {
		buf[got] = '\0';
		ok = !memcmp(buf, "HTTP/1.", 7) && atoi(buf + 9) == 200;
	}
	close(fd);
	return ok;
}

static void *
health_main(void *arg)
{
	struct timespec ts;

	(void)arg;
	pthread_setname_np(pthread_self(), "kache-lb/hc");
	ts.tv_sec = (time_t)(health_ms / 1000);
	ts.tv_nsec = (long)(health_ms % 1000) * 1000000L;
	while (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
		unsigned i;

		for (i = 0; i < nbackends; i++) {
			Backend *b = &backends[i];
			int was = atomic_load_explicit(&b->up,
			                               memory_order_relaxed);
			int now;

			/* Only while it is down, which is also what makes
			 * rewriting b->sa safe without a lock: a worker only
			 * reads it after seeing up, and that read is ordered
			 * against this write by the release store below. */
			if (!was)
				resolve(b);
			now = probe(b);
			atomic_store_explicit(&b->up, now,
			                      memory_order_release);
			if (was != now)
				info("backend %s is %s", b->addr,
				     now ? "up" : "down");
		}
		nanosleep(&ts, NULL);
	}
	return NULL;
}

/* ---- connection pool -------------------------------------------------- */

static void
pool_init(Worker *w, u32 cap)
{
	u32 i;

	w->slots = ecalloc(cap, sizeof(Conn));
	w->cap = cap;
	for (i = 0; i < cap; i++) {
		Conn *c = &w->slots[i];

		c->cside.tag = EV_CLIENT;
		c->cside.conn = c;
		c->bside.tag = EV_BACKEND;
		c->bside.conn = c;
		c->cfd = c->bfd = -1;
		c->fnext = w->freelist;
		w->freelist = c;
	}
}

static void
pool_fini(Worker *w)
{
	u32 i;

	for (i = 0; i < w->cap; i++) {
		free(w->slots[i].c2b);
		free(w->slots[i].b2c);
	}
	free(w->slots);
}

static void
conn_close(Worker *w, Conn *c)
{
	if (!c->open)
		return;
	if (c->cfd >= 0) {
		epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->cfd, NULL);
		close(c->cfd);
		c->cfd = -1;
	}
	if (c->bfd >= 0) {
		epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->bfd, NULL);
		close(c->bfd);
		c->bfd = -1;
	}
	c->open = 0;
	c->fnext = w->freelist;
	w->freelist = c;
	w->used--;
	atomic_store_explicit(&w->closed,
	    atomic_load_explicit(&w->closed, memory_order_relaxed) + 1,
	    memory_order_relaxed);
}

/* ---- the shuttle ------------------------------------------------------
 *
 * Interest follows the buffers, the same one line rule kache's own
 * connections use: a side is read-interested while its outbound buffer
 * has room, and write-interested while the other side's buffer has
 * bytes for it.  That is also the flow control - a slow backend stops
 * the client being read from, and a slow client stops the backend being
 * read from - so there is no separate back pressure mechanism. */
static void
arm(Worker *w, int fd, u32 *cur, u32 want, void *tag)
{
	struct epoll_event ev;

	if (*cur == want)
		return;
	ev.events = want;
	ev.data.ptr = tag;
	epoll_ctl(w->epfd, EPOLL_CTL_MOD, fd, &ev);
	*cur = want;
}

static void
update(Worker *w, Conn *c)
{
	u32 cw = 0, bw = 0;

	if (c->connecting) {
		arm(w, c->bfd, &c->bev, EPOLLOUT, &c->bside);
		return;
	}
	/* read from the client while there is room to put it */
	if (!c->ceof && c->c2b_len < LB_BUF)
		cw |= EPOLLIN;
	/* write to the client while the backend has given us something */
	if (c->b2c_len > c->b2c_off)
		cw |= EPOLLOUT;
	if (!c->beof && c->b2c_len < LB_BUF)
		bw |= EPOLLIN;
	if (c->c2b_len > c->c2b_off)
		bw |= EPOLLOUT;

	arm(w, c->cfd, &c->cev, cw, &c->cside);
	arm(w, c->bfd, &c->bev, bw, &c->bside);
}

/* Move what the kernel has told us is ready, and only that.  Returns -1
 * when the pair is finished with. */
static int
pump(Worker *w, Conn *c)
{
	(void)w;
	for (;;) {
		int moved = 0;
		ssize_t n;

		/* client -> backend */
		if (c->cr && !c->ceof && c->c2b_len < LB_BUF) {
			u32 space = LB_BUF - c->c2b_len;

			n = read(c->cfd, c->c2b + c->c2b_len, space);
			if (n > 0) {
				c->c2b_len += (u32)n;
				moved = 1;
				/* A short read means the socket is drained;
				 * asking again would only earn an EAGAIN. */
				if ((u32)n < space)
					c->cr = 0;
			} else if (n == 0) {
				c->ceof = 1;
				c->cr = 0;
				moved = 1;
			} else {
				c->cr = 0;
				if (errno != EAGAIN && errno != EINTR)
					return -1;
			}
		}
		if (c->bw && c->c2b_len > c->c2b_off) {
			n = write(c->bfd, c->c2b + c->c2b_off,
			          c->c2b_len - c->c2b_off);
			if (n > 0) {
				c->c2b_off += (u32)n;
				moved = 1;
				if (c->c2b_off == c->c2b_len)
					c->c2b_off = c->c2b_len = 0;
				else
					c->bw = 0;   /* the socket filled */
			} else {
				c->bw = 0;
				if (n < 0 && errno != EAGAIN && errno != EINTR)
					return -1;
			}
		}
		/* backend -> client */
		if (c->br && !c->beof && c->b2c_len < LB_BUF) {
			u32 space = LB_BUF - c->b2c_len;

			n = read(c->bfd, c->b2c + c->b2c_len, space);
			if (n > 0) {
				c->b2c_len += (u32)n;
				moved = 1;
				if ((u32)n < space)
					c->br = 0;
			} else if (n == 0) {
				c->beof = 1;
				c->br = 0;
				moved = 1;
			} else {
				c->br = 0;
				if (errno != EAGAIN && errno != EINTR)
					return -1;
			}
		}
		if (c->cw && c->b2c_len > c->b2c_off) {
			n = write(c->cfd, c->b2c + c->b2c_off,
			          c->b2c_len - c->b2c_off);
			if (n > 0) {
				c->b2c_off += (u32)n;
				moved = 1;
				if (c->b2c_off == c->b2c_len)
					c->b2c_off = c->b2c_len = 0;
				else
					c->cw = 0;
			} else {
				c->cw = 0;
				if (n < 0 && errno != EAGAIN && errno != EINTR)
					return -1;
			}
		}

		/* A FIN from one side is forwarded once everything it had
		 * already sent has been handed on, so a client that closes
		 * after its last request still gets the answer to it. */
		if (c->ceof && c->c2b_len == c->c2b_off) {
			shutdown(c->bfd, SHUT_WR);
			if (c->beof && c->b2c_len == c->b2c_off)
				return -1;
		}
		if (c->beof && c->b2c_len == c->b2c_off) {
			shutdown(c->cfd, SHUT_WR);
			if (c->ceof && c->c2b_len == c->c2b_off)
				return -1;
		}
		if (!moved)
			break;
	}
	return 0;
}

static void
do_accept(Worker *w)
{
	int on = 1;

	for (;;) {
		Conn *c;
		int fd, bi, bfd;

		if ((fd = accept4(w->lfd, NULL, NULL,
		                  SOCK_NONBLOCK | SOCK_CLOEXEC)) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

		if (!w->freelist || (bi = pick_backend()) < 0) {
			/* Nothing to hand it to, or nowhere to put it.
			 * Closing at once is the honest answer: holding it
			 * open would only make the client wait to find
			 * out. */
			close(fd);
			atomic_store_explicit(&w->refused,
			    atomic_load_explicit(&w->refused,
			                         memory_order_relaxed) + 1,
			    memory_order_relaxed);
			continue;
		}
		bfd = socket(backends[bi].family,
		             backends[bi].socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
		             backends[bi].protocol);
		if (bfd < 0) {
			close(fd);
			continue;
		}
		setsockopt(bfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
		if (connect(bfd, (struct sockaddr *)&backends[bi].sa,
		            backends[bi].salen) < 0 && errno != EINPROGRESS) {
			atomic_store_explicit(&backends[bi].failed,
			    atomic_load_explicit(&backends[bi].failed,
			                         memory_order_relaxed) + 1,
			    memory_order_relaxed);
			close(bfd);
			close(fd);
			continue;
		}

		c = w->freelist;
		w->freelist = c->fnext;
		w->used++;
		c->cfd = fd;
		c->bfd = bfd;
		c->cev = c->bev = 0;
		c->c2b_len = c->c2b_off = c->b2c_len = c->b2c_off = 0;
		c->ceof = c->beof = 0;
		c->cr = c->cw = c->br = c->bw = 0;
		c->connecting = 1;
		c->open = 1;
		/* Buffers are kept on the slot once allocated: a balancer
		 * churns connections, and freeing 32 KiB to allocate it
		 * again a moment later is work with nothing to show. */
		if (!c->c2b)
			c->c2b = emalloc(LB_BUF);
		if (!c->b2c)
			c->b2c = emalloc(LB_BUF);

		{
			struct epoll_event ev;

			ev.events = 0;
			ev.data.ptr = &c->cside;
			epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev);
			ev.events = EPOLLOUT;
			ev.data.ptr = &c->bside;
			epoll_ctl(w->epfd, EPOLL_CTL_ADD, bfd, &ev);
			c->bev = EPOLLOUT;
		}
		atomic_store_explicit(&w->accepted,
		    atomic_load_explicit(&w->accepted, memory_order_relaxed) + 1,
		    memory_order_relaxed);
		atomic_store_explicit(&backends[bi].opened,
		    atomic_load_explicit(&backends[bi].opened,
		                         memory_order_relaxed) + 1,
		    memory_order_relaxed);
	}
}

static void *
worker_main(void *arg)
{
	Worker *w = arg;
	struct epoll_event evs[LB_EVENTS];
	struct epoll_event ev;
	char name[16];

	snprintf(name, sizeof(name), "kache-lb/%d", w->id);
	pthread_setname_np(pthread_self(), name);

	ev.events = EPOLLIN;
	ev.data.ptr = &w->listen_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->lfd, &ev);
	ev.data.ptr = &w->wake_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->wfd, &ev);

	while (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
		int n = epoll_wait(w->epfd, evs, LB_EVENTS, 200);
		int i;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			warn("epoll_wait:");
			break;
		}
		for (i = 0; i < n; i++) {
			Side *s = evs[i].data.ptr;
			Conn *c;

			if (s->tag == EV_LISTEN) {
				do_accept(w);
				continue;
			}
			if (s->tag == EV_WAKE) {
				u64 junk;
				ssize_t got = read(w->wfd, &junk, sizeof(junk));

				(void)got;
				continue;
			}
			if (!(c = s->conn)->open)
				continue;

			if (s->tag == EV_CLIENT) {
				if (evs[i].events & (EPOLLIN | EPOLLHUP |
				                     EPOLLERR))
					c->cr = 1;
				if (evs[i].events & EPOLLOUT)
					c->cw = 1;
			} else {
				if (evs[i].events & (EPOLLIN | EPOLLHUP |
				                     EPOLLERR))
					c->br = 1;
				if (evs[i].events & EPOLLOUT)
					c->bw = 1;
			}

			if (c->connecting) {
				int err = 0;
				socklen_t el = sizeof(err);

				/* The backend fd becoming writable is how a
				 * non blocking connect reports, and it
				 * reports failure the same way, so the error
				 * has to be asked for rather than assumed. */
				if (getsockopt(c->bfd, SOL_SOCKET, SO_ERROR,
				               &err, &el) < 0 || err) {
					conn_close(w, c);
					continue;
				}
				c->connecting = 0;
				/* A fresh connection is writable and the
				 * client may already have sent its first
				 * request, so let the shuttle try both. */
				c->bw = 1;
				c->cr = 1;
			}
			if (pump(w, c) < 0) {
				conn_close(w, c);
				continue;
			}
			update(w, c);
		}
	}
	return NULL;
}

/* ---- listening -------------------------------------------------------- */

static int
listen_on(const char *addr, const char *port, int backlog)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1, rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	if ((rc = getaddrinfo(addr, port, &hints, &res)) != 0) {
		warn("%s:%s: %s", addr, port, gai_strerror(rc));
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family,
		            ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
		            ai->ai_protocol);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on,
		               sizeof(on)) < 0) {
			warn("SO_REUSEPORT:");
			close(fd);
			fd = -1;
			break;
		}
		if (ai->ai_family == AF_INET6)
			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on,
			           sizeof(on));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(fd, backlog) == 0)
			break;
		warn("%s:%s: bind:", addr, port);
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

/* ---- main -------------------------------------------------------------- */

static const char usage_text[] =
"usage: kache-lb [-qvh] [-l addr] [-p port] [-b backends] [-t threads]\n"
"                [-c conns] [-k ms] [-B backlog]\n"
"\n"
"  -l addr    address to listen on          (default 0.0.0.0)\n"
"  -p port    port to listen on             (default 7080)\n"
"  -b list    backends, host:port,host:port (required)\n"
"  -t n       worker threads, 0 = one/cpu   (default 0)\n"
"  -c n       connections per thread        (default 4096)\n"
"  -k ms      health check interval         (default 1000)\n"
"  -B n       listen backlog                (default 1024)\n"
"  -q         quiet\n"
"  -v         print version and exit\n"
"  -h         this message\n"
"\n"
"A connection level balancer for a kache cluster.  It never parses the\n"
"bytes it carries, which is the point: a pipelined batch crosses it as a\n"
"batch, where an HTTP proxy would replay it one request at a time.\n"
"\n"
"That works because every kache node holds a full copy, so any node can\n"
"answer any read, and because a write that reaches a node which does not\n"
"own the key is answered 307 - so the client is sent straight to the\n"
"owner and never comes back through here.  Start the nodes with -U so\n"
"that redirect names an address the client can reach.\n";

static void
usage(int code)
{
	fputs(usage_text, code ? stderr : stdout);
	exit(code);
}

int
main(int argc, char *argv[])
{
	const char *addr = "0.0.0.0", *port = "7080", *blist = NULL;
	unsigned threads = 0;
	int backlog = 1024, opt;
	unsigned i, started = 0;
	pthread_t hc;
	sigset_t set;
	int sig;

	while ((opt = getopt(argc, argv, "l:p:b:t:c:k:B:qvh")) != -1) {
		switch (opt) {
		case 'l': addr = optarg; break;
		case 'p': port = optarg; break;
		case 'b': blist = optarg; break;
		case 't': threads = (unsigned)atoi(optarg); break;
		case 'c': maxconn = (unsigned)atoi(optarg); break;
		case 'k': health_ms = (unsigned)atoi(optarg); break;
		case 'B': backlog = atoi(optarg); break;
		case 'q': verbosity(0); break;
		case 'v': puts("kache-lb " VERSION); return 0;
		default:  usage(opt == 'h' ? 0 : 2);
		}
	}
	if (!blist || optind != argc)
		usage(2);
	if (!maxconn || !health_ms)
		die("-c and -k must be at least 1");

	{
		char *dup = strdup(blist), *p, *save = NULL;

		if (!dup)
			die("out of memory");
		for (p = strtok_r(dup, ",", &save); p;
		     p = strtok_r(NULL, ",", &save)) {
			if (add_backend(p) < 0)
				die("bad backend: %s", p);
		}
		free(dup);
	}
	if (!nbackends)
		die("-b named no backends");

	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	nworkers = threads ? threads : (unsigned)ncpu();
	workers = ecalloc(nworkers, sizeof(Worker));
	for (i = 0; i < nworkers; i++)
		workers[i].epfd = workers[i].lfd = workers[i].wfd = -1;

	for (i = 0; i < nworkers; i++) {
		Worker *w = &workers[i];

		w->id = (int)i;
		w->listen_tag.tag = EV_LISTEN;
		w->wake_tag.tag = EV_WAKE;
		if ((w->lfd = listen_on(addr, port, backlog)) < 0)
			die("cannot listen on %s:%s", addr, port);
		if ((w->epfd = epoll_create1(EPOLL_CLOEXEC)) < 0)
			die("epoll_create1:");
		if ((w->wfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0)
			die("eventfd:");
		pool_init(w, maxconn);
	}

	pthread_create(&hc, NULL, health_main, NULL);
	for (i = 0; i < nworkers; i++) {
		if (pthread_create(&workers[i].th, NULL, worker_main,
		                   &workers[i]) != 0)
			die("cannot start worker %u", i);
		started++;
	}
	info("kache-lb on %s:%s, %u threads, %u backends", addr, port,
	     nworkers, nbackends);

	if (sigwait(&set, &sig) != 0)
		warn("sigwait:");
	info("shutting down");
	atomic_store_explicit(&stopping, 1, memory_order_relaxed);
	for (i = 0; i < started; i++) {
		u64 one = 1;
		ssize_t put = write(workers[i].wfd, &one, sizeof(one));

		(void)put;
	}
	for (i = 0; i < started; i++)
		pthread_join(workers[i].th, NULL);
	pthread_join(hc, NULL);

	for (i = 0; i < nworkers; i++) {
		if (workers[i].slots)
			pool_fini(&workers[i]);
		if (workers[i].wfd >= 0)
			close(workers[i].wfd);
		if (workers[i].epfd >= 0)
			close(workers[i].epfd);
		if (workers[i].lfd >= 0)
			close(workers[i].lfd);
	}
	free(workers);
	return 0;
}
