/* kache - listening sockets, worker event loops, the ticker, and the
 * startup and shutdown sequence that ties them together. */
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http/conn.h"
#include "http/server.h"
#include "http/stats.h"
#include "util/clk.h"

enum { EV_CONN = 0, EV_LISTEN, EV_WAKE };

typedef struct Tag {
	int tag;
} Tag;

typedef struct Worker {
	Tag       listen_tag;
	Tag       wake_tag;
	int       id;
	int       epfd;
	int       lfd;
	int       wfd;
	Pool      pool;
	Ctx       ctx;
	pthread_t th;
	const ServerCfg *cfg;
} Worker;

static _Atomic int stopping;
static Worker *workers;
static unsigned nworkers;

/* ---- sockets --------------------------------------------------------- */

static int
listen_on(const ServerCfg *cfg)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1, rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	if ((rc = getaddrinfo(cfg->addr, cfg->port, &hints, &res)) != 0) {
		warn("%s:%s: %s", cfg->addr, cfg->port, gai_strerror(rc));
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
		    listen(fd, cfg->backlog) == 0)
			break;
		warn("%s:%s: bind:", cfg->addr, cfg->port);
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

/* ---- worker ---------------------------------------------------------- */

static void
do_accept(Worker *w, u64 now)
{
	int on = 1;

	for (;;) {
		Conn *c;
		int fd = accept4(w->lfd, NULL, NULL,
		                 SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return;       /* EAGAIN, or a transient failure */
		}
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
		if (!(c = conn_open(&w->pool, fd, w->epfd, now))) {
			close(fd);
			st_inc(&w->ctx.st->errors);
			continue;
		}
		st_inc(&w->ctx.st->accepted);
		st_inc(&w->ctx.st->current);
	}
}

static void
reap_idle(Worker *w, u64 now)
{
	u64 cutoff;
	Conn *c;

	if (!w->cfg->idle_ms || now < w->cfg->idle_ms)
		return;
	cutoff = now - w->cfg->idle_ms;
	while ((c = pool_oldest(&w->pool)) != NULL && c->atime <= cutoff)
		conn_close(&w->pool, c, w->ctx.st);
}

static void *
worker_main(void *arg)
{
	Worker *w = arg;
	struct epoll_event evs[CFG_EVENTS];
	struct epoll_event ev;
	u64 last_reap = now_ms();
	char name[16];

	snprintf(name, sizeof(name), "kache/%d", w->id);
	pthread_setname_np(pthread_self(), name);

	ev.events = EPOLLIN;
	ev.data.ptr = &w->listen_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->lfd, &ev);
	ev.data.ptr = &w->wake_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->wfd, &ev);

	while (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
		int n = epoll_wait(w->epfd, evs, CFG_EVENTS, 200);
		u64 now;
		int i;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			warn("epoll_wait:");
			break;
		}
		now = now_ms();
		for (i = 0; i < n; i++) {
			Tag *t = evs[i].data.ptr;

			if (t->tag == EV_LISTEN) {
				do_accept(w, now);
			} else if (t->tag == EV_WAKE) {
				u64 junk;
				ssize_t got = read(w->wfd, &junk,
				                   sizeof(junk));
				(void)got;   /* draining is the point */
			} else {
				Conn *c = (Conn *)t;

				conn_touch(&w->pool, c, now);
				if (conn_event(c, &w->ctx, evs[i].events) < 0)
					conn_close(&w->pool, c, w->ctx.st);
			}
		}
		if (now - last_reap >= 1000) {
			reap_idle(w, now);
			last_reap = now;
		}
	}
	return NULL;
}

/* ---- background ticker ------------------------------------------------ */

static void *
ticker_main(void *arg)
{
	Db *db = arg;
	u64 last_sync = now_ms();
	const ServerCfg *cfg = workers[0].cfg;

	pthread_setname_np(pthread_self(), "kache/tick");
	while (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = (long)CFG_TICK_MS * 1000000L
		};

		nanosleep(&ts, NULL);
		clk_update();
		if (cfg->sync_ms) {
			u64 now = now_ms();

			if (now - last_sync >= cfg->sync_ms) {
				db_sync(db, 0);
				last_sync = now;
			}
		}
	}
	return NULL;
}

/* ---- lifecycle -------------------------------------------------------- */

static void
wake(Worker *w)
{
	u64 one = 1;
	ssize_t put = write(w->wfd, &one, sizeof(one));

	(void)put;   /* the loop re-checks the stop flag anyway */
}

int
server_run(Db *db, const ServerCfg *cfg)
{
	pthread_t ticker;
	sigset_t set;
	unsigned i, started = 0;
	u64 began = now_ms();
	int sig, rc = 0;

	nworkers = cfg->threads ? cfg->threads : (unsigned)ncpu();
	workers = ecalloc(nworkers, sizeof(Worker));
	stats_init(nworkers);

	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	for (i = 0; i < nworkers; i++) {
		Worker *w = &workers[i];

		w->id = (int)i;
		w->cfg = cfg;
		w->listen_tag.tag = EV_LISTEN;
		w->wake_tag.tag = EV_WAKE;
		w->ctx.db = db;
		w->ctx.st = stats_of(i);
		w->ctx.default_ttl = cfg->default_ttl;
		w->ctx.started = began;
		w->ctx.allow_flush = cfg->allow_flush;
		w->ctx.max_req = (size_t)db->map.maxval + CFG_REQ_SLACK;
		w->epfd = -1;
		w->lfd = -1;
		w->wfd = -1;

		if ((w->lfd = listen_on(cfg)) < 0)
			goto fail;
		if ((w->epfd = epoll_create1(EPOLL_CLOEXEC)) < 0) {
			warn("epoll_create1:");
			goto fail;
		}
		if ((w->wfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
			warn("eventfd:");
			goto fail;
		}
		pool_init(&w->pool, cfg->conns);
	}
	for (i = 0; i < nworkers; i++) {
		if (pthread_create(&workers[i].th, NULL, worker_main,
		                   &workers[i]) != 0) {
			warn("cannot start worker %u", i);
			goto fail;
		}
		started++;
	}
	if (pthread_create(&ticker, NULL, ticker_main, db) != 0) {
		warn("cannot start the ticker");
		goto fail;
	}

	info("listening on %s port %s with %u threads", cfg->addr, cfg->port,
	     nworkers);
	if (sigwait(&set, &sig) != 0)
		warn("sigwait:");
	info("shutting down");
	atomic_store_explicit(&stopping, 1, memory_order_relaxed);

	for (i = 0; i < nworkers; i++)
		wake(&workers[i]);
	for (i = 0; i < started; i++)
		pthread_join(workers[i].th, NULL);
	pthread_join(ticker, NULL);
	goto out;

fail:
	rc = -1;
	atomic_store_explicit(&stopping, 1, memory_order_relaxed);
	for (i = 0; i < started; i++) {
		wake(&workers[i]);
		pthread_join(workers[i].th, NULL);
	}
out:
	for (i = 0; i < nworkers; i++) {
		Worker *w = &workers[i];

		if (w->pool.slots)
			pool_fini(&w->pool);
		if (w->wfd >= 0)
			close(w->wfd);
		if (w->epfd >= 0)
			close(w->epfd);
		if (w->lfd >= 0)
			close(w->lfd);
	}
	stats_fini();
	free(workers);
	workers = NULL;
	return rc;
}
