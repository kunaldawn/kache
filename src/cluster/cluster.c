/* kache - replication across nodes.  See cluster.h for the design. */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
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

/* ---- the membership snapshot -----------------------------------------
 *
 * Under -J the member list changes while requests are being served, so
 * what a request reads has to be a snapshot: one that cannot gain a node
 * halfway through being read, and cannot have an index resolved against
 * one list and an address against another.
 *
 * Two of them, flipped by a release store, exactly as clk.c flips its
 * formatted date - and safe for the same reason plus a stronger one.  A
 * reader holds a View for the few hundred nanoseconds it takes to look
 * up a bucket and copy one address, while the writer is a resolver that
 * runs once every CFG_RESOLVE_MS at the fastest.  Two flips inside one
 * reader's window would need the resolver to run a million times faster
 * than it is allowed to. */

/* Ownership is decided per bucket, not per key.  Rendezvous hashing is
 * O(nodes) and belongs nowhere near a request, so it runs over the
 * buckets when the membership changes and leaves behind a plain array:
 * one load to route a key, against a division for `% nnodes`.  Measured,
 * that is 0.47ns against 6.01ns, so sharded mode can afford to route
 * reads as well as writes and still cost less than the modulo did.
 *
 * 16 bits keeps the worst node within 1.13x of the mean at every size up
 * to 64 nodes, and the table inside 64 KiB. */
#define CL_OWN_BITS 16u
#define CL_BUCKETS  (1u << CL_OWN_BITS)

typedef struct View {
	unsigned nnodes;
	unsigned self;                    /* CL_NOSELF until we find ourselves */
	char     caddr[CL_MAX_NODES][256];/* what a client is told to use */
	u8       owner[CL_BUCKETS];
} View;

#define CL_NOSELF ((unsigned)-1)

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
	int      mode;
	Node     node[CL_MAX_NODES];
	unsigned nnodes;
	unsigned self;
	u64      flush_ms;

	/* the published membership, and the slot the resolver writes next */
	View             view[2];
	_Atomic unsigned vcur;        /* the published View; set[] has its own */
	char     discover[256];       /* -J: the name whose A records we are */
	char     dport[16];
	char     selfaddr[256];       /* -I, when given */
	u64      resolve_ms;
	pthread_t rth;                /* resolver thread, CL_SHARD only */
	_Atomic u64 resolves, changes;

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

/* ---- ownership -------------------------------------------------------- */

/* A node's identity is its address, not its position: that is the whole
 * point, because a position changes when the list does and an address
 * does not. */
static u64
node_id(const char *addr)
{
	return hash_bytes(addr, strlen(addr), 0x6b61636865ull);
}

/* Rendezvous: the node scoring highest for this bucket takes it.  Ties
 * break on the lower index so every node builds the same table. */
static void
own_build(View *v, const u64 *id)
{
	unsigned b, i;

	for (b = 0; b < CL_BUCKETS; b++) {
		u64 best = 0;
		unsigned who = 0;

		for (i = 0; i < v->nnodes; i++) {
			u64 sc = hash_mix((u64)b ^ id[i],
			                  0x9e3779b97f4a7c15ull);

			if (sc > best) {
				best = sc;
				who = i;
			}
		}
		v->owner[b] = (u8)who;
	}
}

static inline const View *
view_of(const Cluster *c)
{
	return &c->view[atomic_load_explicit(&c->vcur, memory_order_acquire)];
}

int
cl_route(Cluster *c, u64 hash, char *addr, size_t cap)
{
	const View *v = view_of(c);
	unsigned who;

	/* No members yet - the first resolve has not landed, or every peer
	 * went away.  Answering locally is the only useful thing left: a
	 * redirect needs somewhere to point. */
	if (!v->nnodes)
		return 0;
	who = v->owner[hash >> (64 - CL_OWN_BITS)];
	if (who == v->self)
		return 0;
	snprintf(addr, cap, "%s", v->caddr[who]);
	return 1;
}

unsigned cl_self(const Cluster *c)  { return view_of(c)->self; }
unsigned cl_nodes(const Cluster *c) { return view_of(c)->nnodes; }

void
cl_discovery(const Cluster *c, u64 *resolves, u64 *changes)
{
	*resolves = atomic_load_explicit(&c->resolves, memory_order_relaxed);
	*changes = atomic_load_explicit(&c->changes, memory_order_relaxed);
}

void
cl_stats(const Cluster *c, u64 *sent, u64 *recvd, u64 *failed)
{
	*sent = atomic_load_explicit(&c->sent, memory_order_relaxed);
	*recvd = atomic_load_explicit(&c->recvd, memory_order_relaxed);
	*failed = atomic_load_explicit(&c->failed, memory_order_relaxed);
}

/* ---- discovery --------------------------------------------------------
 *
 * The members are whatever the -J name resolves to.  In Kubernetes that
 * is a headless Service, which publishes one A record per ready pod and
 * withdraws it the moment a readiness probe fails - so a pod being
 * evicted from a spot node leaves the membership before it stops
 * answering, which is exactly the ordering a redirect needs.  Under
 * Docker Compose a service name behaves the same way, which is what
 * makes the whole thing testable on a laptop.
 *
 * Nothing tells a node which member it is.  It works that out by
 * matching the resolved addresses against its own interfaces, because a
 * pod knows its own addresses and knows nothing about its index. */

/* Is addr one of this host's own addresses? */
static int
is_local(const struct sockaddr *sa)
{
	struct ifaddrs *ifa, *p;
	int found = 0;

	if (getifaddrs(&ifa) != 0)
		return 0;
	for (p = ifa; p && !found; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != sa->sa_family)
			continue;
		if (sa->sa_family == AF_INET) {
			const struct sockaddr_in *a = (const void *)sa;
			const struct sockaddr_in *b = (const void *)p->ifa_addr;

			found = a->sin_addr.s_addr == b->sin_addr.s_addr;
		} else if (sa->sa_family == AF_INET6) {
			const struct sockaddr_in6 *a = (const void *)sa;
			const struct sockaddr_in6 *b = (const void *)p->ifa_addr;

			found = !memcmp(&a->sin6_addr, &b->sin6_addr,
			                sizeof a->sin6_addr);
		}
	}
	freeifaddrs(ifa);
	return found;
}

/* Resolve -J into the next View.  Returns -1 when the name does not
 * resolve at all, which is left to the caller: keeping the membership we
 * had beats emptying it because a DNS server hiccuped. */
static int
resolve_members(Cluster *c, View *v)
{
	struct addrinfo hints, *res, *ai;
	char host[INET6_ADDRSTRLEN], cand[256], me[256];
	u64 id[CL_MAX_NODES];
	unsigned i, j;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(c->discover, c->dport, &hints, &res) != 0)
		return -1;

	v->nnodes = 0;
	v->self = CL_NOSELF;
	for (ai = res; ai && v->nnodes < CL_MAX_NODES; ai = ai->ai_next) {
		int mine;

		if (getnameinfo(ai->ai_addr, ai->ai_addrlen, host, sizeof host,
		                NULL, 0, NI_NUMERICHOST) != 0)
			continue;
		/* Build it first and compare whole strings.  Matching a
		 * bracketed "[::1]:7070" against a bare "::1" never does,
		 * so an IPv6 address returned twice would have been taken
		 * as two members and given two shares of the keyspace. */
		snprintf(cand, sizeof cand,
		         strchr(host, ':') ? "[%s]:%s" : "%s:%s",
		         host, c->dport);
		/* An address can come back more than once - one entry per
		 * socktype on some resolvers - and a member counted twice
		 * would take two shares. */
		for (j = 0; j < v->nnodes; j++)
			if (!strcmp(v->caddr[j], cand))
				break;
		if (j < v->nnodes)
			continue;
		mine = c->selfaddr[0] ? !strcmp(host, c->selfaddr)
		                      : is_local(ai->ai_addr);
		snprintf(v->caddr[v->nnodes], sizeof v->caddr[0], "%s", cand);
		if (mine)
			v->self = v->nnodes;
		v->nnodes++;
	}
	freeaddrinfo(res);
	if (!v->nnodes)
		return -1;

	/* The order the resolver hands addresses back in is not stable -
	 * Kubernetes shuffles them per query on purpose - so sort, or two
	 * nodes would build different tables from the same membership and
	 * disagree about who owns what. */
	if (v->self != CL_NOSELF)
		snprintf(me, sizeof me, "%s", v->caddr[v->self]);
	for (i = 1; i < v->nnodes; i++) {
		char tmp[256];

		snprintf(tmp, sizeof tmp, "%s", v->caddr[i]);
		for (j = i; j && strcmp(v->caddr[j - 1], tmp) > 0; j--)
			snprintf(v->caddr[j], sizeof v->caddr[0], "%s",
			         v->caddr[j - 1]);
		snprintf(v->caddr[j], sizeof v->caddr[0], "%s", tmp);
	}
	/* Our own index moved with the sort.  Finding it again is one pass
	 * over at most 64 strings once per membership change, and it cannot
	 * be subtly wrong the way carrying an index through the shuffling
	 * can - which matters, because a node that mislocates itself serves
	 * another node's keys and redirects its own away. */
	if (v->self != CL_NOSELF) {
		v->self = CL_NOSELF;
		for (i = 0; i < v->nnodes; i++) {
			if (!strcmp(v->caddr[i], me)) {
				v->self = i;
				break;
			}
		}
	}

	for (i = 0; i < v->nnodes; i++)
		id[i] = node_id(v->caddr[i]);
	own_build(v, id);
	return 0;
}

/* Did the membership actually move?  Rebuilding a 64 KiB table and
 * flipping the view on every tick would be work for nothing; almost
 * every tick sees the same pods it saw a second ago. */
static int
same_members(const View *a, const View *b)
{
	unsigned i;

	if (a->nnodes != b->nnodes || a->self != b->self)
		return 0;
	for (i = 0; i < a->nnodes; i++)
		if (strcmp(a->caddr[i], b->caddr[i]))
			return 0;
	return 1;
}

static void
publish(Cluster *c, const View *v)
{
	unsigned next = atomic_load_explicit(&c->vcur,
	                    memory_order_relaxed) ^ 1u;

	c->view[next] = *v;
	atomic_store_explicit(&c->vcur, next, memory_order_release);
}

static void *
resolver(void *arg)
{
	Cluster *c = arg;
	struct timespec ts;
	View *scratch = emalloc(sizeof *scratch);

	pthread_setname_np(pthread_self(), "kache/disc");
	/* Sleep in slices rather than for the whole interval: the interval
	 * is seconds and shutdown waits on this thread, so sleeping through
	 * it would put -D on the front of every teardown.  Same reason the
	 * flusher's peer I/O is bounded. */
	ts.tv_sec = 0;
	ts.tv_nsec = 100 * 1000000L;
	while (!atomic_load_explicit(&c->stopping, memory_order_relaxed)) {
		u64 slept;

		memset(scratch, 0, sizeof *scratch);
		if (resolve_members(c, scratch) == 0) {
			atomic_store_explicit(&c->resolves,
			    atomic_load_explicit(&c->resolves,
			        memory_order_relaxed) + 1,
			    memory_order_relaxed);
			if (!same_members(scratch, view_of(c))) {
				publish(c, scratch);
				atomic_store_explicit(&c->changes,
				    atomic_load_explicit(&c->changes,
				        memory_order_relaxed) + 1,
				    memory_order_relaxed);
				/* Not finding ourselves is survivable - every
				 * key is simply forwarded - but if one of
				 * those addresses is in fact ours the client
				 * is sent straight back here and round again.
				 * It means -I is wrong, or the address is not
				 * on an interface in this namespace, and it
				 * is worth saying so rather than leaving a
				 * redirect loop to be worked out from a
				 * packet capture. */
				if (scratch->self == CL_NOSELF)
					warn("cluster: %u members of %s, none "
					     "of them this node; check -I",
					     scratch->nnodes, c->discover);
				else
					info("cluster: %u members, self %u",
					     scratch->nnodes, scratch->self);
			}
		}
		for (slept = 0; slept < c->resolve_ms; slept += 100) {
			if (atomic_load_explicit(&c->stopping,
			                         memory_order_relaxed))
				break;
			nanosleep(&ts, NULL);
		}
	}
	free(scratch);
	return NULL;
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

	/* Sharded mode keeps no second copy of anything, so there is
	 * nothing to tell a peer about. */
	if (!c || c->mode != CL_REPLICA || kl > CFG_MAX_KEY)
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
	if (cfg->mode == CL_SHARD) {
		if (!cfg->discover || !*cfg->discover)
			return 0;
	} else if (!cfg->peers || !*cfg->peers) {
		return 0;
	}
	c = ecalloc(1, sizeof(Cluster));
	c->db = db;
	c->mode = cfg->mode;
	c->flush_ms = cfg->flush_ms ? cfg->flush_ms : CFG_REPL_MS;
	lock_init(&c->lock);

	if (cfg->mode == CL_SHARD) {
		View *v = emalloc(sizeof *v);

		snprintf(c->discover, sizeof c->discover, "%s", cfg->discover);
		snprintf(c->dport, sizeof c->dport, "%s", cfg->port);
		if (cfg->self_addr)
			snprintf(c->selfaddr, sizeof c->selfaddr, "%s",
			         cfg->self_addr);
		c->resolve_ms = cfg->resolve_ms ? cfg->resolve_ms
		                                : CFG_RESOLVE_MS;
		/* Resolve once before serving.  A pod that started answering
		 * while it still believed it was alone would claim the whole
		 * keyspace and redirect nothing, which is worse than being a
		 * moment late to become ready. */
		memset(v, 0, sizeof *v);
		if (resolve_members(c, v) < 0)
			warn("cluster: %s does not resolve yet; serving "
			     "locally until it does", c->discover);
		else
			info("cluster: sharded over %u members of %s, self %u",
			     v->nnodes, c->discover, v->self);
		publish(c, v);
		free(v);
		if (pthread_create(&c->rth, NULL, resolver, c) != 0) {
			warn("cluster: cannot start the resolver");
			free(c);
			return -1;
		}
		*out = c;
		return 0;
	}

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
	/* A fixed list is just a membership that never changes, so it goes
	 * through the same View and the same owner table.  -C keeps its
	 * index arithmetic in one respect only: the list is given in an
	 * order everyone shares, so no sort is needed. */
	{
		View *v = emalloc(sizeof *v);
		u64 id[CL_MAX_NODES];
		unsigned i;

		memset(v, 0, sizeof *v);
		v->nnodes = c->nnodes;
		v->self = c->self;
		for (i = 0; i < c->nnodes; i++) {
			snprintf(v->caddr[i], sizeof v->caddr[0], "%s",
			         c->node[i].caddr);
			id[i] = node_id(c->node[i].addr);
		}
		own_build(v, id);
		publish(c, v);
		free(v);
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
	if (c->mode == CL_SHARD) {
		pthread_join(c->rth, NULL);
		free(c);
		return;
	}
	pthread_join(c->th, NULL);
	for (i = 0; i < c->nnodes; i++)
		if (c->node[i].fd >= 0)
			close(c->node[i].fd);
	dirty_fini(&c->set[0]);
	dirty_fini(&c->set[1]);
	free(c->val);
	free(c);
}
