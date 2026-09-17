/* kache - listening sockets, worker event loops, and the startup and
 * shutdown sequence that ties them together. */
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
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
	u64       lresume;      /* ms at which a paused listener is re-armed */
	Pool      pool;
	Ctx       ctx;
	pthread_t th;
	const ServerCfg *cfg;
} Worker;

/* How long the listener stays out of the epoll set after the process has
 * run out of descriptors.  Level triggered interest means a listener with
 * a pending connection it cannot accept reports readable on every pass,
 * so without this the worker spins on accept4 at a hundred percent of a
 * core for as long as the shortage lasts - exactly when the machine can
 * least afford it.  Long enough that the spin is gone, short enough that
 * recovery is not noticeable. */
#define ACCEPT_PAUSE_MS 100u

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

/* Worker i takes cpu i % ncpu(), and takes it from inside the thread so
 * its stack and its first buffers are faulted in where they will stay.
 * Placing the loops by hand only pays when nothing else on the box wants
 * those cpus, so it is off by default, and a kernel that refuses is not
 * a reason not to serve.  It assumes one worker per logical cpu, so
 * pairing it with CFG_THREADS_SMT 0 pins to logical ids, not to cores. */
static void
pin(const Worker *w)
{
	cpu_set_t set;
	int cpu = w->id % ncpu();
	int rc;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if ((rc = pthread_setaffinity_np(pthread_self(), sizeof(set),
	                                 &set)) != 0) {
		errno = rc;
		warn("worker %d: cannot pin to cpu %d:", w->id, cpu);
	}
}

static void
wake(Worker *w)
{
	u64 one = 1;
	ssize_t put = write(w->wfd, &one, sizeof(one));

	(void)put;   /* the loop re-checks the stop flag anyway */
}

/* ---- worker ---------------------------------------------------------- */

/* Take the listener out of, or put it back into, this worker's epoll. */
static void
listen_arm(Worker *w, int on)
{
	struct epoll_event ev;

	ev.events = on ? EPOLLIN : 0;
	ev.data.ptr = &w->listen_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_MOD, w->lfd, &ev);
}

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
			/* Out of descriptors or out of memory: the pending
			 * connection stays pending and the listener stays
			 * readable, so coming straight back is a busy loop.
			 * Stand down briefly instead. */
			if (errno == EMFILE || errno == ENFILE ||
			    errno == ENOBUFS || errno == ENOMEM) {
				st_inc(&w->ctx.st->errors);
				listen_arm(w, 0);
				w->lresume = now + ACCEPT_PAUSE_MS;
			}
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
	u64 last_reap = now_ms(), last_sync = last_reap;
	char name[16];

	snprintf(name, sizeof(name), "kache/%d", w->id);
	pthread_setname_np(pthread_self(), name);

	ev.events = EPOLLIN;
	ev.data.ptr = &w->listen_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->lfd, &ev);
	ev.data.ptr = &w->wake_tag;
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->wfd, &ev);

	if (w->cfg->affinity)
		pin(w);
	while (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
		int n = epoll_wait(w->epfd, evs, CFG_EVENTS, 200);
		int err = errno;
		u64 now;
		int i;

		/* Whoever comes out of epoll_wait refreshes the clock, so
		 * no thread exists to do only that.  Under load it is
		 * fresher than a millisecond timer would manage, and idle
		 * it trails by at most one timeout, which nobody can
		 * observe because nobody is asking.  errno is saved first
		 * because clk_read_ms may clobber it on its fallback. */
		clk_update();
		if (n < 0) {
			if (err == EINTR)
				continue;
			errno = err;
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
		if (UNLIKELY(w->lresume) && now >= w->lresume) {
			w->lresume = 0;
			listen_arm(w, 1);
		}
		if (now - last_reap >= 1000) {
			reap_idle(w, now);
			/* Give back what deleting a container left behind.
			 * Requests do this too, on their way in, but only
			 * to the shard they were going to touch anyway - a
			 * store nobody is asking about would otherwise
			 * hold the memory until something needed it. */
			db_reclaim(w->ctx.db, (u32)w->id, nworkers,
			           CFG_RECLAIM_BUDGET);
			last_reap = now;
		}
		/* One worker carries the periodic flush for all of them.
		 * It gets its own deadline rather than riding the reap
		 * tick above, which would quietly round -y up to a
		 * second; MS_ASYNC only queues the writeback, so the
		 * pause it costs this loop is short. */
		if (w->id == 0 && w->cfg->sync_ms &&
		    now - last_sync >= w->cfg->sync_ms) {
			db_sync(w->ctx.db, 0);
			last_sync = now;
		}
	}
	/* A worker only leaves that loop on a shutdown or on an epoll it
	 * cannot use again.  In the second case the others would carry on
	 * serving with one listener unattended and, if this was worker 0,
	 * with nothing flushing the store, so take the whole server down
	 * rather than half of it. */
	if (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
		unsigned i;

		warn("worker %d stopped; shutting down", w->id);
		atomic_store_explicit(&stopping, 1, memory_order_relaxed);
		for (i = 0; i < nworkers; i++)
			wake(&workers[i]);
	}
	return NULL;
}

/* ---- lifecycle -------------------------------------------------------- */

int
server_run(Db *db, const ServerCfg *cfg)
{
	sigset_t set;
	unsigned i, started = 0;
	u64 began = now_ms();
	int sig, rc = 0;

	/* This loop is memory bound, so a second worker on a core's
	 * sibling can be worth less than the cache lines it evicts;
	 * CFG_THREADS_SMT 0 asks for one worker per physical core. */
	nworkers = cfg->threads ? cfg->threads :
	           (unsigned)(CFG_THREADS_SMT ? ncpu() : ncores());
	workers = ecalloc(nworkers, sizeof(Worker));
	stats_init(nworkers);

	/* Every worker's descriptors have to read as unset before the
	 * first goto: the cleanup at out: walks all of them, not only
	 * the ones the setup loop below reached. */
	for (i = 0; i < nworkers; i++)
		workers[i].epfd = workers[i].lfd = workers[i].wfd = -1;

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
		w->ctx.minimal = cfg->minimal;
		w->ctx.cl = cfg->cl;
		w->ctx.max_req = (size_t)db->map.maxval + CFG_REQ_SLACK;
		/* Per worker and never shared, which is the whole point of
		 * it; allocated here so the memory is faulted in by the
		 * thread that will own it once affinity has placed it. */
		if (cfg->hot_ms)
			w->ctx.hot = hot_new(began, cfg->hot_ms);

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
		if (w->ctx.hot)
			hot_free(w->ctx.hot);
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
