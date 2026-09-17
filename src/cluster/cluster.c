/* kache - replication across nodes.  See cluster.h for the design. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "cluster/cluster.h"
#include "config.h"
#include "http/buf.h"
#include "store/db.h"
#include "util/clk.h"
#include "util/hash.h"
#include "util/lock.h"
#include "util/util.h"

typedef struct Node {
	char  addr[256];      /* "host:port", as given */
	char  caddr[256];     /* what a client is told to use, see -U */
	char  host[256];
	char  port[16];
	int   fd;             /* kept open between flushes, -1 when down */
	u64   failed;
} Node;

/* The dirty set is a plain open addressed table of keys.  It is written
 * by workers taking writes and drained by the flusher, so it is the one
 * structure here that needs a lock - but a write is rare next to a read
 * on any key worth replicating, and the critical section is a hash and a
 * memcpy, so the same argument the shard lock makes applies. */
typedef struct Dirty {
	u8   *key;            /* CL_DIRTY_SLOTS * (CFG_MAX_KEY + 1) */
	u16  *klen;           /* 0 marks an empty slot */
	u32   n;
} Dirty;

struct Cluster {
	Db      *db;
	Node     node[CL_MAX_NODES];
	unsigned nnodes;
	unsigned self;
	u64      flush_ms;

	Lock     lock;        /* covers cur */
	Dirty    set[2];      /* one filling, one being sent */
	int      cur;

	pthread_t th;
	u8      *val;         /* the flusher's scratch value buffer, one
	                       * allocation for the life of the process
	                       * rather than one per flush window */
	_Atomic int stopping;

	_Atomic u64 sent, recvd, failed;
};

/* Slots in one dirty set.  A node only ever replicates the keys it owns
 * and that are actually being written, so this is sized for a working
 * set of hot keys, not for the store.  A full set drops the least
 * recently offered key rather than growing: losing a replication of a
 * key that is being overwritten faster than it can be shipped costs a
 * staleness window, which is what the caller already accepted. */
#define CL_DIRTY_SLOTS 4096u

/* Frame buffer kept between flushes; anything above it is a spike and is
 * handed back rather than held for the life of the process. */
#define CL_BODY_KEEP (1u << 20)

/* ---- membership ------------------------------------------------------ */

/* Copy the n'th comma separated field of s into dst.  Returns 0 when
 * there is no n'th field, which is how a short -U list is caught. */
static int
nth_field(const char *s, unsigned n, char *dst, size_t cap)
{
	const char *p = s;
	unsigned i;

	for (i = 0; i < n; i++) {
		if (!(p = strchr(p, ',')))
			return 0;
		p++;
	}
	{
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);

		if (!len || len >= cap)
			return 0;
		memcpy(dst, p, len);
		dst[len] = '\0';
	}
	return 1;
}

static int
parse_peers(Cluster *c, const char *s)
{
	const char *p = s;

	while (*p && c->nnodes < CL_MAX_NODES) {
		const char *comma = strchr(p, ',');
		size_t n = comma ? (size_t)(comma - p) : strlen(p);
		Node *nd = &c->node[c->nnodes];
		const char *colon;

		if (!n || n >= sizeof(nd->addr)) {
			warn("cluster: bad node address");
			return -1;
		}
		memcpy(nd->addr, p, n);
		nd->addr[n] = '\0';
		/* Split on the last colon so a bracketed IPv6 literal keeps
		 * its own.  Anything without one is missing a port, and
		 * guessing one would put half a cluster on a port nobody
		 * meant. */
		if (!(colon = strrchr(nd->addr, ':'))) {
			warn("cluster: %s has no port", nd->addr);
			return -1;
		}
		n = (size_t)(colon - nd->addr);
		memcpy(nd->host, nd->addr, n);
		nd->host[n] = '\0';
		snprintf(nd->port, sizeof(nd->port), "%s", colon + 1);
		nd->fd = -1;
		/* Defaults to the peer address, so a cluster that needs no
		 * advertised list behaves exactly as it did. */
		memcpy(nd->caddr, nd->addr, sizeof(nd->caddr));
		c->nnodes++;
		if (!comma)
			break;
		p = comma + 1;
	}
	return c->nnodes ? 0 : -1;
}

unsigned
cl_owner(const Cluster *c, u64 hash)
{
	/* The top bits, for the same reason the store picks its shard
	 * from them: the low bits are already the bucket, so reusing
	 * them would line node ownership up with a structure the keys
	 * are deliberately spread across. */
	return (unsigned)((hash >> 40) % c->nnodes);
}

int
cl_is_mine(const Cluster *c, u64 hash)
{
	return cl_owner(c, hash) == c->self;
}

const char *
cl_addr(const Cluster *c, unsigned node)
{
	return node < c->nnodes ? c->node[node].addr : "";
}

const char *
cl_client_addr(const Cluster *c, unsigned node)
{
	return node < c->nnodes ? c->node[node].caddr : "";
}

unsigned cl_self(const Cluster *c)  { return c->self; }
unsigned cl_nodes(const Cluster *c) { return c->nnodes; }

void
cl_stats(const Cluster *c, u64 *sent, u64 *recvd, u64 *failed)
{
	*sent = atomic_load_explicit(&c->sent, memory_order_relaxed);
	*recvd = atomic_load_explicit(&c->recvd, memory_order_relaxed);
	*failed = atomic_load_explicit(&c->failed, memory_order_relaxed);
}

/* ---- the dirty set --------------------------------------------------- */

static int
dirty_init(Dirty *d)
{
	d->key = ecalloc(CL_DIRTY_SLOTS, CFG_MAX_KEY + 1);
	d->klen = ecalloc(CL_DIRTY_SLOTS, sizeof(u16));
	d->n = 0;
	return 0;
}

static void
dirty_fini(Dirty *d)
{
	free(d->key);
	free(d->klen);
}

static inline u8 *
dirty_at(Dirty *d, u32 i)
{
	return d->key + (size_t)i * (CFG_MAX_KEY + 1);
}

/* Records the key if it is not already there.  That test is the whole
 * point of the structure: it is what turns a write storm on one key into
 * one shipped value per flush. */
static void
dirty_put(Dirty *d, u64 hash, const void *k, u32 kl)
{
	u32 i = (u32)(hash & (CL_DIRTY_SLOTS - 1)), probe;

	for (probe = 0; probe < CL_DIRTY_SLOTS; probe++) {
		u32 s = (i + probe) & (CL_DIRTY_SLOTS - 1);

		if (!d->klen[s]) {
			memcpy(dirty_at(d, s), k, kl);
			d->klen[s] = (u16)kl;
			d->n++;
			return;
		}
		if (d->klen[s] == kl && !memcmp(dirty_at(d, s), k, kl))
			return;      /* already pending; the value is read later */
	}
	/* Full.  See the note on CL_DIRTY_SLOTS. */
}

static void
dirty_clear(Dirty *d)
{
	memset(d->klen, 0, CL_DIRTY_SLOTS * sizeof(u16));
	d->n = 0;
}

void
cl_dirty(Cluster *c, const void *k, u32 kl)
{
	u64 h;

	if (!c || kl > CFG_MAX_KEY)
		return;
	h = hash_bytes(k, kl, c->db->map.seed);
	lock_acquire(&c->lock);
	dirty_put(&c->set[c->cur], h, k, kl);
	lock_release(&c->lock);
}

/* ---- the wire format -------------------------------------------------
 *
 * One frame per key, length prefixed for the same reason the batch
 * endpoints are: it is the only framing that keeps a value binary safe.
 *
 *   <op> <klen> <vlen> <ttl_ms> <flags> <version>\n<key><value>
 *
 * op is S for a value and D for a deletion.  A deletion has to be on the
 * wire in its own right: the flusher reads each dirty key's value at send
 * time, so a key that was deleted simply is not there any more, and
 * without a tombstone the peers would keep the copy they already had for
 * ever.
 *
 * The header line is text because it is written once per key and read
 * once per key, and a text line is the thing that can be read out of a
 * tcpdump when a cluster is misbehaving at three in the morning. */

static void
frame(Buf *b, int op, const void *k, u32 kl, const void *v, u32 vl,
      const DbMeta *m)
{
	buf_putc(b, (char)op);
	buf_putc(b, ' ');
	buf_putu(b, kl);
	buf_putc(b, ' ');
	buf_putu(b, vl);
	buf_putc(b, ' ');
	buf_puti(b, m->ttl);
	buf_putc(b, ' ');
	buf_putu(b, m->flags);
	buf_putc(b, ' ');
	buf_putu(b, m->version);
	buf_putc(b, '\n');
	buf_put(b, k, kl);
	if (vl)
		buf_put(b, v, vl);
}

/* One space separated field of a frame header, bounded by the line it
 * sits on.  The bound is the whole point: these bytes arrive off a
 * socket and carry no terminator, so a parser that scans for one reads
 * past the request. */
static int
wire_field(const char **p, const char *end, const char **f, size_t *fn)
{
	const char *s = *p, *sp;

	while (s < end && *s == ' ')
		s++;
	if (s == end)
		return -1;
	for (sp = s; sp < end && *sp != ' '; sp++)
		;
	*f = s;
	*fn = (size_t)(sp - s);
	*p = sp;
	return 0;
}

static int
wire_u64(const char **p, const char *end, u64 *out)
{
	const char *f;
	size_t fn;

	if (wire_field(p, end, &f, &fn) < 0)
		return -1;
	return parse_u64(f, fn, out);
}

static int
wire_i64(const char **p, const char *end, i64 *out)
{
	const char *f;
	size_t fn;

	if (wire_field(p, end, &f, &fn) < 0)
		return -1;
	return parse_i64(f, fn, out);
}

int
cl_apply(Cluster *c, const void *body, size_t n, u32 *applied)
{
	const char *p = body, *end = p + n;

	*applied = 0;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *line, *f;
		u64 kl, vl, flags, version;
		size_t fn;
		i64 ttl;
		DbMeta m;
		char op;
		int rc;

		if (!nl)
			return -1;
		/* Every field is read inside this one line, and every length
		 * is checked against what is really left of the body.  The
		 * frame header is the only part of kache a peer writes and
		 * this node parses, so it is the only place where being
		 * strict costs nothing and being loose costs everything. */
		line = p;
		if (wire_field(&line, nl, &f, &fn) < 0 || fn != 1)
			return -1;
		op = *f;
		if (op != 'S' && op != 'D')
			return -1;
		if (wire_u64(&line, nl, &kl) < 0 ||
		    wire_u64(&line, nl, &vl) < 0 ||
		    wire_i64(&line, nl, &ttl) < 0 ||
		    wire_u64(&line, nl, &flags) < 0 ||
		    wire_u64(&line, nl, &version) < 0)
			return -1;
		/* nothing but spaces may follow the last field */
		if (wire_field(&line, nl, &f, &fn) == 0)
			return -1;

		p = nl + 1;
		if (kl == 0 || kl > CFG_MAX_KEY || vl > c->db->map.maxval ||
		    flags > 0xffffffffull ||
		    (u64)(end - p) < kl + vl)
			return -1;
		/* The owner's version rides the wire because it is the first
		 * thing wanted when a cluster is disagreeing, but it is not
		 * and cannot be the merge rule: a replica's db_set assigns a
		 * version from its own shard counter, so the two numbers are
		 * not comparable.  What orders these writes is that they have
		 * one source and travel one connection in order. */
		(void)version;
		/* Applied as an ordinary write, deliberately.  The owner is
		 * the only source of these, and one connection carries them
		 * in order, so the last one to arrive is the last one sent
		 * and no comparison is needed.  What must not happen is
		 * cl_dirty being called here - that is what would send the
		 * value back where it came from, for ever. */
		if (op == 'D') {
			/* A tombstone is applied without CAS: it carries the
			 * owner's decision, and the replica has no standing to
			 * argue with it. */
			rc = db_del(c->db, p, (u32)kl, 0, 0);
			if (rc == DB_OK || rc == DB_ENOENT)
				(*applied)++;
		} else {
			rc = db_set(c->db, p, (u32)kl, p + kl, (u32)vl, (i64)ttl,
			            (u32)flags, SET_ANY, 0, &m);
			if (rc == DB_OK)
				(*applied)++;
		}
		p += (size_t)(kl + vl);
	}
	atomic_store_explicit(&c->recvd,
	    atomic_load_explicit(&c->recvd, memory_order_relaxed) + *applied,
	    memory_order_relaxed);
	return 0;
}

/* ---- the peer connection --------------------------------------------- */

/* Every peer operation is bounded in time, because all of them happen on
 * the one flusher thread.  A peer that accepts a connection and then says
 * nothing - a machine being fenced, a dropped route, a box that is
 * swapping - would otherwise park the thread in the kernel and stop this
 * node replicating to the peers that are still healthy.  A timeout turns
 * that into one missed window against one peer. */
#define CL_IO_MS 2000

static void
peer_deadline(int fd)
{
	struct timeval tv;

	tv.tv_sec = CL_IO_MS / 1000;
	tv.tv_usec = (CL_IO_MS % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* A blocking connect() to an address that black holes packets takes the
 * kernel's SYN retry budget to fail - over two minutes - and the flusher
 * has nothing else to do meanwhile.  So the socket is non blocking for
 * the connect alone and waited on with our own deadline, then put back. */
static int
connect_wait(int fd, const struct sockaddr *sa, socklen_t salen)
{
	struct pollfd pfd;
	int flags, err = 0;
	socklen_t el = sizeof(err);

	if ((flags = fcntl(fd, F_GETFL, 0)) < 0)
		return -1;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	if (connect(fd, sa, salen) < 0) {
		if (errno != EINPROGRESS)
			return -1;
		pfd.fd = fd;
		pfd.events = POLLOUT;
		for (;;) {
			int rc = poll(&pfd, 1, CL_IO_MS);

			if (rc > 0)
				break;
			if (rc == 0 || errno != EINTR)
				return -1;
		}
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err)
			return -1;
	}
	return fcntl(fd, F_SETFL, flags);
}

static int
peer_connect(Node *n)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1, on = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(n->host, n->port, &hints, &res) != 0)
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
		            ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect_wait(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0) {
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
		peer_deadline(fd);
	}
	n->fd = fd;
	return fd;
}

/* The request line and the frames go out together.  Two write() calls
 * are two syscalls and, with TCP_NODELAY set, usually two segments on
 * the wire for what is one message - so a flush of a handful of small
 * keys costs twice the packets it needs to.  One writev is one of
 * each. */
static int
writev_all(int fd, struct iovec *v, int n)
{
	while (n) {
		ssize_t w = writev(fd, v, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		while (n && (size_t)w >= v->iov_len) {
			w -= (ssize_t)v->iov_len;
			v++;
			n--;
		}
		if (n && w) {
			v->iov_base = (char *)v->iov_base + w;
			v->iov_len -= (size_t)w;
		}
	}
	return 0;
}

/* Reads until the end of the status line and takes the code from it.
 * The reply to a replication POST is a 204 with no body, so there is
 * nothing after the headers to drain beyond them. */
static int
read_status(int fd)
{
	char b[512];
	size_t n = 0;
	int code = -1;

	for (;;) {
		ssize_t r = read(fd, b + n, sizeof(b) - n - 1);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return -1;
		}
		n += (size_t)r;
		b[n] = '\0';
		if (code < 0 && n > 12 && !memcmp(b, "HTTP/1.", 7))
			code = atoi(b + 9);
		if (n >= 4 && memmem(b, n, "\r\n\r\n", 4))
			return code;
		if (n >= sizeof(b) - 1)
			return code;
	}
}

static void
peer_send(Cluster *c, Node *n, const Buf *body)
{
	char head[512];
	struct iovec v[2];
	int len, code;

	if (n->fd < 0 && peer_connect(n) < 0) {
		n->failed++;
		atomic_store_explicit(&c->failed,
		    atomic_load_explicit(&c->failed, memory_order_relaxed) + 1,
		    memory_order_relaxed);
		return;
	}
	len = snprintf(head, sizeof(head),
	    "POST /x/repl HTTP/1.1\r\nHost: %s\r\n"
	    "Content-Length: %zu\r\n\r\n", n->addr, body->len);
	/* One connection per peer, written sequentially, is what makes
	 * the ordering argument in cl_apply true.  A failure closes it so
	 * the next flush reconnects rather than resuming a stream whose
	 * position nobody knows. */
	v[0].iov_base = head;
	v[0].iov_len = (size_t)len;
	v[1].iov_base = body->p;
	v[1].iov_len = body->len;
	if (writev_all(n->fd, v, 2) < 0 ||
	    (code = read_status(n->fd)) < 0 || code >= 400) {
		close(n->fd);
		n->fd = -1;
		n->failed++;
		atomic_store_explicit(&c->failed,
		    atomic_load_explicit(&c->failed, memory_order_relaxed) + 1,
		    memory_order_relaxed);
	}
}

/* ---- the flusher ----------------------------------------------------- */

/* Swap the set the workers are filling for the empty one, so the send
 * happens with nothing holding the lock.  This is the only reason there
 * are two of them. */
static Dirty *
swap_sets(Cluster *c)
{
	Dirty *full;

	lock_acquire(&c->lock);
	full = &c->set[c->cur];
	c->cur ^= 1;
	lock_release(&c->lock);
	return full;
}

static void
flush(Cluster *c, Buf *body)
{
	Dirty *d = swap_sets(c);
	u8 *val = c->val;
	u32 i, nsent = 0;

	if (!d->n)
		return;
	body->len = body->off = 0;
	for (i = 0; i < CL_DIRTY_SLOTS; i++) {
		DbMeta m;
		u8 *k;

		if (!d->klen[i])
			continue;
		k = dirty_at(d, i);
		/* The value is read now, not when the key was written.
		 * That is the coalescing: every write inside this window
		 * collapses into whatever the key holds at this moment. */
		if (db_get(c->db, k, d->klen[i], val, c->db->map.maxval,
		           &m) == DB_OK) {
			frame(body, 'S', k, d->klen[i], val, m.vlen, &m);
		} else {
			/* Gone from here, so it has to go from the peers too.
			 * A key that expired rather than being deleted lands
			 * here as well, which is harmless: the peers hold the
			 * same expiry and would have dropped it themselves. */
			memset(&m, 0, sizeof(m));
			m.ttl = DB_FOREVER;
			frame(body, 'D', k, d->klen[i], NULL, 0, &m);
		}
		nsent++;
	}
	dirty_clear(d);
	if (nsent) {
		unsigned p;

		for (p = 0; p < c->nnodes; p++) {
			if (p == c->self)
				continue;
			peer_send(c, &c->node[p], body);
		}
		atomic_store_explicit(&c->sent,
		    atomic_load_explicit(&c->sent, memory_order_relaxed) + nsent,
		    memory_order_relaxed);
	}
	/* One window that happened to carry large values would otherwise
	 * leave the frame buffer that size for the life of the process, and
	 * the flusher never shrinks on its own.  Keeping the common case
	 * allocated and giving back the spike is the same trade the
	 * connection buffers make. */
	if (body->cap > CL_BODY_KEEP)
		buf_free(body);
}

static void *
flusher(void *arg)
{
	Cluster *c = arg;
	Buf body;
	struct timespec ts;

	memset(&body, 0, sizeof(body));
	pthread_setname_np(pthread_self(), "kache/repl");
	ts.tv_sec = (time_t)(c->flush_ms / 1000);
	ts.tv_nsec = (long)(c->flush_ms % 1000) * 1000000L;
	while (!atomic_load_explicit(&c->stopping, memory_order_relaxed)) {
		nanosleep(&ts, NULL);
		flush(c, &body);
	}
	flush(c, &body);   /* what was pending when the signal arrived */
	buf_free(&body);
	return NULL;
}

/* ---- lifecycle -------------------------------------------------------- */

int
cl_open(Cluster **out, Db *db, const ClusterCfg *cfg)
{
	Cluster *c;

	*out = NULL;
	if (!cfg->peers || !*cfg->peers)
		return 0;
	c = ecalloc(1, sizeof(Cluster));
	c->db = db;
	c->flush_ms = cfg->flush_ms ? cfg->flush_ms : CFG_REPL_MS;
	lock_init(&c->lock);
	if (parse_peers(c, cfg->peers) < 0) {
		free(c);
		return -1;
	}
	if (cfg->self >= c->nnodes) {
		warn("cluster: -N %u names no node in a list of %u",
		     cfg->self, c->nnodes);
		free(c);
		return -1;
	}
	c->self = cfg->self;
	if (cfg->advertise && *cfg->advertise) {
		unsigned i;

		for (i = 0; i < c->nnodes; i++) {
			if (!nth_field(cfg->advertise, i, c->node[i].caddr,
			               sizeof(c->node[i].caddr))) {
				warn("cluster: -U lists fewer than the %u "
				     "nodes in -C", c->nnodes);
				free(c);
				return -1;
			}
		}
	}
	dirty_init(&c->set[0]);
	dirty_init(&c->set[1]);
	c->val = emalloc(db->map.maxval);
	if (pthread_create(&c->th, NULL, flusher, c) != 0) {
		warn("cluster: cannot start the flusher");
		dirty_fini(&c->set[0]);
		dirty_fini(&c->set[1]);
		free(c->val);
		free(c);
		return -1;
	}
	info("cluster: node %u of %u, %s, flushing every %llums",
	     c->self, c->nnodes, c->node[c->self].addr,
	     (unsigned long long)c->flush_ms);
	*out = c;
	return 0;
}

void
cl_close(Cluster *c)
{
	unsigned i;

	if (!c)
		return;
	atomic_store_explicit(&c->stopping, 1, memory_order_relaxed);
	pthread_join(c->th, NULL);
	for (i = 0; i < c->nnodes; i++)
		if (c->node[i].fd >= 0)
			close(c->node[i].fd);
	dirty_fini(&c->set[0]);
	dirty_fini(&c->set[1]);
	free(c->val);
	free(c);
}
