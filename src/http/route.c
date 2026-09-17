/* kache - the mapping from request to store operation, and back to a
 * status code.  This is the only file that knows the HTTP contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "http/route.h"
#include "util/clk.h"
#include "util/hash.h"

#define KEYMAX CFG_MAX_KEY

#define CT_BIN  "application/octet-stream"
#define CT_TXT  "text/plain; charset=utf-8"

typedef struct Key {
	char c[KEYMAX + 1];
	u32  n;
} Key;

/* ---- small response helpers ----------------------------------------- */

/* A response to HEAD ends with its header block: a body left behind would
 * be read as the start of the next response on a kept alive connection.
 * Content-Length still describes what a GET would have returned. */
static void
no_body(Buf *out, size_t mark, const Hdrs *h, int head)
{
	if (head)
		out->len = mark + h->n;
}

static void
reply_text(Ctx *c, Buf *out, int status, int ka, const char *msg, size_t n)
{
	size_t mark = out->len;
	Hdrs h;

	buf_put(out, msg, n);
	buf_putc(out, '\n');
	hdrs_start(&h, status, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_TXT);
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
	no_body(out, mark, &h, c->head);
}

static void
fail(Ctx *c, Buf *out, int status, int ka, const char *msg)
{
	st_inc(&c->st->errors);
	reply_text(c, out, status, ka, msg, strlen(msg));
}

/* every failure the store can report, mapped onto a status */
static int
status_of(int rc)
{
	switch (rc) {
	case DB_ENOENT: return 404;
	case DB_EEXIST: return 412;
	case DB_ECAS:   return 412;
	case DB_ENOSPC: return 507;
	case DB_E2BIG:  return 413;
	case DB_ENUM:   return 409;
	case DB_ETYPE:  return 409;
	}
	return 500;
}

static void
fail_db(Ctx *c, Buf *out, int rc, int ka)
{
	fail(c, out, status_of(rc), ka, db_strerror(rc));
}

/* ---- request helpers ------------------------------------------------ */

static int
prefixed(Str path, const char *pfx, size_t n, Str *rest)
{
	if (path.n < n || memcmp(path.p, pfx, n))
		return 0;
	rest->p = path.p + n;
	rest->n = path.n - n;
	return 1;
}

/* -1 malformed escape or empty, -2 longer than the key limit */
static int
take_key(Str rest, Key *k)
{
	int n;

	if (!rest.n)
		return -1;
	if (rest.n > KEYMAX * 3)
		return -2;
	/* url_decode costs a bounds check per byte and hardly any key
	 * carries an escape.  Without one the decoded length is the raw
	 * length, so the same inputs still fail with the same codes.  The
	 * length test has to stay in here: an escaped key may be up to
	 * three times KEYMAX raw and still decode to a legal one. */
	if (LIKELY(!memchr(rest.p, '%', rest.n))) {
		if (rest.n > KEYMAX)
			return -2;
		memcpy(k->c, rest.p, rest.n);
		k->c[rest.n] = '\0';
		k->n = (u32)rest.n;
		return 0;
	}
	if ((n = url_decode(rest.p, rest.n, k->c, sizeof(k->c) - 1)) < 0)
		return rest.n > KEYMAX ? -2 : -1;
	if (n == 0)
		return -1;
	k->c[n] = '\0';
	k->n = (u32)n;
	return 0;
}

/* ttl in seconds from the query or the header, 0 meaning "no expiry".
 * Returns 1 when the request carried one, 0 when it did not, -1 on junk.
 * The names are parameters because a container has two independent
 * expiries: ttl / ttlms for the item, kttl / kttlms for the key it lives
 * in.  Only the item one has a header spelling. */
static int
take_ttl_of(const Req *r, const char *sec, const char *ms, Str hdr,
            i64 dfl, i64 *out)
{
	i64 v;
	int rc;

	if ((rc = query_i64(r->query, ms, &v)) == 0) {
		if (v < 0)
			return -1;
		*out = v ? v : DB_FOREVER;
		return 1;
	}
	if (rc == -2)
		return -1;
	if ((rc = query_i64(r->query, sec, &v)) == 0) {
		if (v < 0 || v > INT64_MAX / 1000)
			return -1;
		*out = v ? v * 1000 : DB_FOREVER;
		return 1;
	}
	if (rc == -2)
		return -1;
	if (hdr.n) {
		if (parse_i64(hdr.p, hdr.n, &v) < 0 || v < 0 ||
		    v > INT64_MAX / 1000)
			return -1;
		*out = v ? v * 1000 : DB_FOREVER;
		return 1;
	}
	*out = dfl;
	return 0;
}

static int
take_ttl(const Req *r, i64 dfl, i64 *out)
{
	return take_ttl_of(r, "ttl", "ttlms", r->xttl, dfl, out);
}

/* the container's own expiry, which no header spells */
static int
take_kttl(const Req *r, i64 dfl, i64 *out)
{
	Str none = { NULL, 0 };

	return take_ttl_of(r, "kttl", "kttlms", none, dfl, out);
}

static int
take_flags(const Req *r, u32 *out)
{
	u64 v;
	int rc;

	*out = 0;
	if ((rc = query_u32(r->query, "flags", out)) == 0)
		return 0;
	if (rc == -2)
		return -1;
	if (r->xflags.n) {
		if (parse_u64(r->xflags.p, r->xflags.n, &v) < 0 ||
		    v > 0xffffffffull)
			return -1;
		*out = (u32)v;
	}
	return 0;
}

static void
meta_headers(Hdrs *h, const DbMeta *m)
{
	hdrs_etag(h, m->version);
	hdrs_num(h, "X-Kache-TTL",
	         m->ttl < 0 ? -1 : (i64)((m->ttl + 999) / 1000));
	hdrs_num(h, "X-Kache-Flags", (i64)m->flags);
}

/* ---- handlers ------------------------------------------------------- */

/* Whether this request's answer is one the hot set may keep and replay.
 * A cached response is replayed byte for byte with only its Date
 * refreshed, so anything that could vary it has to be excluded here
 * rather than tested at replay time: a HEAD withholds the body, an
 * HTTP/1.0 client needs the keep-alive echo that minimal mode drops, and
 * a close response carries a Connection header the next client will not
 * want.  A conditional request is excluded although reading does not act
 * on If-Match or If-None-Match today - it is the one exclusion guarding
 * something that does not exist yet, and it is here so that teaching the
 * read path to answer 304 does not silently start replaying a 200. */
static inline int
hot_ok(const Ctx *c, const Req *r, int ka, const Key *k)
{
	return c->hot && r->meth == M_GET && ka && r->minor &&
	       !r->ifmatch.n && !r->ifnone.n && k->n <= CFG_HOT_KEY;
}


/* ---- cluster routing -------------------------------------------------
 *
 * Reads are answered by whichever node they reach, out of that node's own
 * copy, which is the whole point: a hot key is on every node and no read
 * ever waits for a peer.  Writes are not.  A key's writes belong to one
 * owner, fixed by the hash, so that replicas receive a single ordered
 * stream and there is no conflict to resolve; a write that lands
 * anywhere else is sent to the owner rather than applied here.
 *
 * It is a redirect and not a forward.  Forwarding would make this node
 * hold a request open while it waits on another one, which is how a slow
 * peer turns into this node's queue; a redirect hands the decision back
 * to the client, which can also remember it and stop guessing wrong.
 * 307 is the one that preserves the method and the body. */
static int
elsewhere(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	char loc[512];
	unsigned owner;
	u64 h;
	size_t n;
	Hdrs hd;

	if (!c->cl)
		return 0;
	h = hash_bytes(k->c, k->n, c->db->map.seed);
	if (cl_is_mine(c->cl, h))
		return 0;
	owner = cl_owner(c->cl, h);
	/* The path is echoed back exactly as it arrived, still percent
	 * encoded, so a key that needed escaping stays escaped. */
	n = (size_t)snprintf(loc, sizeof(loc), "http://%s%.*s%s%.*s",
	    cl_client_addr(c->cl, owner), (int)r->path.n, r->path.p,
	    r->query.n ? "?" : "", (int)r->query.n, r->query.p);
	if (n >= sizeof(loc)) {
		fail(c, out, 414, ka, "redirect target too long");
		return 1;
	}
	hdrs_start(&hd, 307, clk_date(), c->minimal_req);
	hdrs_add(&hd, "Location", loc, n);
	hdrs_end(&hd, 0, ka);
	buf_put(out, hd.b, hd.n);
	return 1;
}

/* Tell the flusher this node changed a key.  Called after the write has
 * landed, never on the path that applies one arriving from a peer. */
static inline void
replicate(Ctx *c, const Key *k)
{
	if (c->cl)
		cl_dirty(c->cl, k->c, k->n);
}

static void
do_get(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	size_t mark = out->len;
	DbMeta m;
	Hdrs h;
	u8 nothing;
	u64 hash = 0, now = 0;
	int rc, tries, cache = 0, head = (r->meth == M_HEAD);

	if (hot_ok(c, r, ka, k)) {
		const HotEnt *e;

		now = now_ms();
		hash = hash_bytes(k->c, k->n, c->db->map.seed);
		if ((e = hot_get(c->hot, hash, k->c, k->n, now)) != NULL) {
			/* The whole answer, headers and body, in one copy.
			 * buf_put may reallocate, so the Date is patched
			 * through the buffer after the copy rather than
			 * through a pointer taken before it. */
			buf_put(out, e->resp, e->rlen);
			memcpy(out->p + mark + e->doff, clk_date(),
			       CLK_DATE_LEN);
			st_inc(&c->st->hits);
			st_inc(&c->st->hot_hits);
			return;
		}
		cache = 1;
	}

	if (head) {
		rc = db_get(c->db, k->c, k->n, &nothing, 0, &m);
		if (rc == DB_ESMALL)
			rc = DB_OK;
	} else {
		for (tries = 0; tries < 4; tries++) {
			u32 cap;

			if (buf_room(out) < 512)
				buf_grow(out, 512, 0);
			cap = (u32)MIN(buf_room(out), (size_t)0xffffffffu);
			rc = db_get(c->db, k->c, k->n, buf_tail(out), cap, &m);
			if (rc != DB_ESMALL)
				break;
			buf_grow(out, m.vlen, 0);
		}
		if (rc == DB_OK)
			out->len += m.vlen;
	}
	if (rc != DB_OK) {
		st_inc(&c->st->misses);
		if (rc == DB_ENOENT)
			reply_text(c, out, 404, ka, "not found", 9);
		else
			fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->hits);
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	/* Minimal mode withholds the read side metadata, and only here:
	 * hdrs_start and hdrs_end drop Server and the keep-alive echo from
	 * every response, but a write still answers with the ETag and TTL
	 * that feed CAS.  A HEAD keeps them too - it returns no bytes, so
	 * the elision would save nothing, and metadata is the whole point
	 * of asking. */
	if (!c->minimal_req || head) {
		hdrs_lit(&h, "Content-Type", CT_BIN);
		meta_headers(&h, &m);
	}
	hdrs_end(&h, m.vlen, ka);
	http_wrap(out, mark, &h);

	/* The response is finished and contiguous from mark, which is the
	 * one moment it can be kept whole.  Admission is asked only here,
	 * on a path that has already missed, so a key that is in the set
	 * never touches the doorkeeper at all. */
	if (cache) {
		u32 rlen = (u32)(out->len - mark);
		int doff;

		if (rlen <= CFG_HOT_RESP &&
		    (doff = hot_date_off(out->p + mark, rlen)) >= 0 &&
		    hot_admit(c->hot, hash, now)) {
			hot_fill(c->hot, hash, k->c, k->n, out->p + mark,
			         rlen, (u32)doff, now, m.ttl);
			st_inc(&c->st->hot_fills);
		}
	}
}

static void
do_put(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	DbMeta m;
	Hdrs h;
	i64 ttl;
	u64 cas = 0;
	u32 flags;
	int mode = SET_ANY, rc;

	if (elsewhere(c, r, out, ka, k))
		return;
	/* This worker just changed a value, so its own cached answers
	 * are suspect.  Retiring the whole set is one store; finding
	 * the one entry would cost a hash of the written key on every
	 * write, to retire at most one. */
	if (c->hot)
		hot_dirty(c->hot);

	if (r->ifnone.n) {
		if (r->ifnone.n != 1 || r->ifnone.p[0] != '*') {
			fail(c, out, 400, ka, "only If-None-Match: * is supported");
			return;
		}
		mode = SET_ADD;
	} else if (r->ifmatch.n) {
		if (r->ifmatch.n == 1 && r->ifmatch.p[0] == '*') {
			mode = SET_REPLACE;
		} else if (etag_value(r->ifmatch, &cas) < 0) {
			fail(c, out, 400, ka, "malformed If-Match");
			return;
		} else {
			mode = SET_CAS;
		}
	}
	if (take_ttl(r, c->default_ttl, &ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	if (take_flags(r, &flags) < 0) {
		fail(c, out, 400, ka, "bad flags");
		return;
	}

	rc = db_set(c->db, k->c, k->n, r->body.p, (u32)r->body.n,
	            ttl, flags, mode, cas, &m);
	if (rc != DB_OK) {
		/* a conditional request that found nothing to match
		 * failed its precondition, it did not 404 */
		if (rc == DB_ENOENT && mode != SET_ANY)
			fail(c, out, 412, ka, "precondition failed");
		else
			fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->sets);
	replicate(c, k);
	hdrs_start(&h, m.created ? 201 : 204, clk_date(), c->minimal_req);
	meta_headers(&h, &m);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
do_del(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	u64 cas = 0;
	int use_cas = 0, rc;

	if (elsewhere(c, r, out, ka, k))
		return;
	/* This worker just changed a value, so its own cached answers
	 * are suspect.  Retiring the whole set is one store; finding
	 * the one entry would cost a hash of the written key on every
	 * write, to retire at most one. */
	if (c->hot)
		hot_dirty(c->hot);

	if (r->ifmatch.n && !(r->ifmatch.n == 1 && r->ifmatch.p[0] == '*')) {
		if (etag_value(r->ifmatch, &cas) < 0) {
			fail(c, out, 400, ka, "malformed If-Match");
			return;
		}
		use_cas = 1;
	}
	rc = db_del(c->db, k->c, k->n, use_cas, cas);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->dels);
	replicate(c, k);
	http_simple(out, 204, clk_date(), ka, c->minimal_req);
}

static void
do_incr(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, int sign)
{
	size_t mark = out->len;
	DbMeta m;
	Hdrs h;
	i64 delta = 1, init = 0, ttl, result;
	int set_ttl, rc;

	if (elsewhere(c, r, out, ka, k))
		return;
	/* This worker just changed a value, so its own cached answers
	 * are suspect.  Retiring the whole set is one store; finding
	 * the one entry would cost a hash of the written key on every
	 * write, to retire at most one. */
	if (c->hot)
		hot_dirty(c->hot);

	if (query_i64(r->query, "by", &delta) == -2) {
		fail(c, out, 400, ka, "bad by");
		return;
	}
	if (query_i64(r->query, "init", &init) == -2) {
		fail(c, out, 400, ka, "bad init");
		return;
	}
	if ((set_ttl = take_ttl(r, c->default_ttl, &ttl)) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	if (sign < 0) {
		if (delta == INT64_MIN) {
			fail(c, out, 409, ka, "delta out of range");
			return;
		}
		delta = -delta;
	}

	rc = db_incr(c->db, k->c, k->n, delta, init, ttl, set_ttl, &result, &m);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->incrs);
	replicate(c, k);
	buf_puti(out, result);
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_TXT);
	meta_headers(&h, &m);
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
}

static void
do_cat(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, int prepend)
{
	DbMeta m;
	Hdrs h;
	int rc;

	if (elsewhere(c, r, out, ka, k))
		return;
	/* This worker just changed a value, so its own cached answers
	 * are suspect.  Retiring the whole set is one store; finding
	 * the one entry would cost a hash of the written key on every
	 * write, to retire at most one. */
	if (c->hot)
		hot_dirty(c->hot);

	rc = db_cat(c->db, k->c, k->n, r->body.p, (u32)r->body.n, prepend, &m);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->cats);
	replicate(c, k);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	meta_headers(&h, &m);
	hdrs_num(&h, "X-Kache-Length", (i64)m.vlen);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
do_touch(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	DbMeta m;
	Hdrs h;
	i64 ttl;
	int rc;

	if (elsewhere(c, r, out, ka, k))
		return;
	/* This worker just changed a value, so its own cached answers
	 * are suspect.  Retiring the whole set is one store; finding
	 * the one entry would cost a hash of the written key on every
	 * write, to retire at most one. */
	if (c->hot)
		hot_dirty(c->hot);

	if (take_ttl(r, c->default_ttl, &ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	if ((rc = db_touch(c->db, k->c, k->n, ttl, &m)) != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->touches);
	replicate(c, k);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	meta_headers(&h, &m);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

/* ---- introspection --------------------------------------------------- */

typedef struct Metric {
	const char *name;
	const char *type;
	u64         v;
} Metric;

static size_t
collect(Ctx *c, Metric *mt, size_t cap)
{
	enum { REQ, HIT, MISS, SET, DEL, INCR, CAT, TOUCH, ERR,
	       ACCEPT, CLOSE, CUR, BIN, BOUT,
	       KKVR, KKVW, KKVD, QPUSH, QPOP, HOTH, HOTF };
	u64 s[STATS_FIELDS];
	DbStats d;
	size_t n = 0;

	db_stats(c->db, &d);
	stats_sum(s, STATS_FIELDS);

#define M(nm, ty, val) do { \
	if (n < cap) { mt[n].name = (nm); mt[n].type = (ty); \
	               mt[n].v = (u64)(val); n++; } \
} while (0)
	M("uptime_ms", "gauge", now_ms() - c->started);
	M("keys", "gauge", d.keys);
	M("bytes_used", "gauge", d.bytes);
	M("bytes_capacity", "gauge", d.capacity);
	M("buckets", "gauge", d.buckets);
	M("inserts_total", "counter", d.inserts);
	M("evictions_total", "counter", d.evictions);
	M("expirations_total", "counter", d.expirations);
	M("requests_total", "counter", s[REQ]);
	M("hits_total", "counter", s[HIT]);
	M("misses_total", "counter", s[MISS]);
	M("sets_total", "counter", s[SET]);
	M("deletes_total", "counter", s[DEL]);
	M("increments_total", "counter", s[INCR]);
	M("appends_total", "counter", s[CAT]);
	M("touches_total", "counter", s[TOUCH]);
	M("errors_total", "counter", s[ERR]);
	M("connections", "gauge", s[CUR]);
	M("connections_accepted_total", "counter", s[ACCEPT]);
	M("connections_closed_total", "counter", s[CLOSE]);
	M("bytes_in_total", "counter", s[BIN]);
	M("bytes_out_total", "counter", s[BOUT]);
	M("map_reads_total", "counter", s[KKVR]);
	M("map_writes_total", "counter", s[KKVW]);
	M("map_deletes_total", "counter", s[KKVD]);
	M("queue_pushes_total", "counter", s[QPUSH]);
	M("queue_pops_total", "counter", s[QPOP]);
	if (c->cl) {
		u64 sent, recvd, failed;

		cl_stats(c->cl, &sent, &recvd, &failed);
		M("cluster_nodes", "gauge", cl_nodes(c->cl));
		M("cluster_self", "gauge", cl_self(c->cl));
		/* Frames shipped, not writes taken.  The gap between this
		 * and sets_total is the coalescing doing its job, and it is
		 * the number to look at when peer traffic is a worry. */
		M("repl_sent_total", "counter", sent);
		M("repl_received_total", "counter", recvd);
		M("repl_failures_total", "counter", failed);
	}
	M("hot_hits_total", "counter", s[HOTH]);
	M("hot_fills_total", "counter", s[HOTF]);
	M("reclaim_pending", "gauge", d.dead);
#undef M
	return n;
}

static void
do_stats(Ctx *c, Buf *out, int ka, int prom)
{
	Metric mt[40];
	size_t n = collect(c, mt, LEN(mt)), i;
	size_t mark = out->len;
	Hdrs h;

	if (!prom) {
		buf_puts(out, "version ");
		buf_puts(out, VERSION);
		buf_putc(out, '\n');
	}
	for (i = 0; i < n; i++) {
		if (prom) {
			buf_puts(out, "# TYPE kache_");
			buf_puts(out, mt[i].name);
			buf_putc(out, ' ');
			buf_puts(out, mt[i].type);
			buf_puts(out, "\nkache_");
		}
		buf_puts(out, mt[i].name);
		buf_putc(out, ' ');
		buf_putu(out, mt[i].v);
		buf_putc(out, '\n');
	}
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_TXT);
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
	no_body(out, mark, &h, c->head);
}

static const char index_page[] =
"kache " VERSION "\n"
"\n"
"  GET    /kv/<key>            fetch, ETag carries the CAS token\n"
"  HEAD   /kv/<key>            metadata only\n"
"  PUT    /kv/<key>            store the body\n"
"  POST   /kv/<key>            same as PUT\n"
"  DELETE /kv/<key>            remove\n"
"  POST   /incr/<key>          add ?by=N (default 1) to a numeric value\n"
"  POST   /decr/<key>          subtract ?by=N\n"
"  POST   /append/<key>        append the body\n"
"  POST   /prepend/<key>       prepend the body\n"
"  POST   /touch/<key>         reset the ttl\n"
"  POST   /mget                one key per line, framed values back\n"
"  POST   /mset                \"<key> <bytes> [ttl]\" then the bytes\n"
"  POST   /mdel                one key per line\n"
"  POST   /flush               drop everything\n"
"  GET    /stats               counters, one per line\n"
"  GET    /metrics             the same in prometheus form\n"
"  GET    /health              liveness\n"
"\n"
"nested maps - the key holds fields, and both levels carry a ttl\n"
"\n"
"  GET    /kkv/<key>           dump every field, framed\n"
"  GET    /kkv/<key>?f=<fld>   one field\n"
"  PUT    /kkv/<key>?f=<fld>   store one field\n"
"  POST   /kkv/<key>           set many fields at once, atomically\n"
"  DELETE /kkv/<key>?f=<fld>   remove one field\n"
"  DELETE /kkv/<key>           remove the whole map\n"
"  POST   /kkvdel/<key>        remove many fields, framed names\n"
"  POST   /kkvincr/<key>?f=    add ?by=N to a field\n"
"  POST   /kkvdecr/<key>?f=    subtract ?by=N\n"
"  POST   /kkvtouch/<key>[?f=] reset a field ttl, or the map's\n"
"\n"
"queues - a deque with a ttl on the queue and one on every message\n"
"\n"
"  POST   /q/<key>             push the body, ?side=r by default\n"
"  POST   /qpush/<key>         push many messages, framed\n"
"  POST   /qpop/<key>          pop ?n (default 1), ?side=l by default\n"
"  GET    /q/<key>             the same without removing anything\n"
"  POST   /qmove/<key>?dst=<k> pop one and push it, under both locks\n"
"  POST   /qtrim/<key>?maxlen= keep at most that many\n"
"  POST   /qtouch/<key>        reset the queue ttl\n"
"  DELETE /q/<key>             remove the queue\n"
"\n"
"  ?ttl=<seconds>              0 means never expire\n"
"  ?ttlms=<milliseconds>       finer grained ttl\n"
"  ?flags=<u32>                opaque, returned in X-Kache-Flags\n"
"  If-None-Match: *            store only if absent\n"
"  If-Match: \"<etag>\"          store or delete only on that version\n"
"  ?kttl=<seconds>             the container's own ttl, not the item's\n"
"  ?f=<field>                  percent encoded, so any bytes go\n"
"  ?n=<count>                  how many to return; frames the answer\n"
"\n"
"frames are length prefixed, so names and values are binary safe:\n"
"  map:   \"<fieldlen> <valuelen> [ttl]\\n\" <field> <value> \"\\n\"\n"
"  queue: \"<valuelen> [ttl]\\n\" <value> \"\\n\"\n";

/* ---- dispatch -------------------------------------------------------- */

static void do_mget(Ctx *c, const Req *r, Buf *out, int ka, int del);
static void do_mset(Ctx *c, const Req *r, Buf *out, int ka);
static void do_kkv(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_kkv_incr(Ctx *c, const Req *r, Buf *out, int ka, const Key *k,
                        int sign);
static void do_kkv_touch(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_kkv_mdel(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_q(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_q_push(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_q_pop(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_q_move(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_q_trim(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);
static void do_q_touch(Ctx *c, const Req *r, Buf *out, int ka, const Key *k);

static int
key_or_fail(Ctx *c, Buf *out, int ka, Str rest, Key *k)
{
	int rc = take_key(rest, k);

	if (rc == -2) {
		fail(c, out, 414, ka, "key too long");
		return -1;
	}
	if (rc < 0) {
		fail(c, out, 400, ka, "bad key");
		return -1;
	}
	return 0;
}

void
route(Ctx *c, const Req *r, Buf *out, int *keepalive)
{
	int ka = r->keepalive;
	Str rest;
	Key k;

	st_inc(&c->st->requests);
	*keepalive = ka;
	/* An HTTP/1.0 client only keeps the connection when the server
	 * echoes Connection: keep-alive, and minimal mode is exactly the
	 * mode that stops echoing it.  So it never applies to one. */
	c->minimal_req = c->minimal && r->minor;
	c->head = (r->meth == M_HEAD);

	if (prefixed(r->path, "/kv/", 4, &rest)) {
		if (key_or_fail(c, out, ka, rest, &k) < 0)
			return;
		switch (r->meth) {
		case M_GET:
		case M_HEAD:   do_get(c, r, out, ka, &k); return;
		case M_PUT:
		case M_POST:   do_put(c, r, out, ka, &k); return;
		case M_DELETE: do_del(c, r, out, ka, &k); return;
		}
		fail(c, out, 405, ka, "method not allowed");
		return;
	}
	if (prefixed(r->path, "/kkv/", 5, &rest)) {
		if (key_or_fail(c, out, ka, rest, &k) < 0)
			return;
		do_kkv(c, r, out, ka, &k);
		return;
	}
	if (prefixed(r->path, "/q/", 3, &rest)) {
		if (key_or_fail(c, out, ka, rest, &k) < 0)
			return;
		do_q(c, r, out, ka, &k);
		return;
	}
	if (r->meth == M_POST) {
		static const struct {
			const char *pfx;
			size_t      n;
			void      (*fn)(Ctx *, const Req *, Buf *, int,
			                const Key *);
		} cont[] = {
			{ "/kkvtouch/", 10, do_kkv_touch },
			{ "/kkvdel/",    8, do_kkv_mdel },
			{ "/qpush/",     7, do_q_push },
			{ "/qpop/",      6, do_q_pop },
			{ "/qmove/",     7, do_q_move },
			{ "/qtrim/",     7, do_q_trim },
			{ "/qtouch/",    8, do_q_touch }
		};
		int sign = 0, prepend = -1;
		size_t ci;

		for (ci = 0; ci < LEN(cont); ci++) {
			if (!prefixed(r->path, cont[ci].pfx, cont[ci].n, &rest))
				continue;
			if (key_or_fail(c, out, ka, rest, &k) < 0)
				return;
			cont[ci].fn(c, r, out, ka, &k);
			return;
		}
		if (prefixed(r->path, "/kkvincr/", 9, &rest))
			sign = 1;
		else if (prefixed(r->path, "/kkvdecr/", 9, &rest))
			sign = -1;
		if (sign) {
			if (key_or_fail(c, out, ka, rest, &k) < 0)
				return;
			do_kkv_incr(c, r, out, ka, &k, sign);
			return;
		}

		if (prefixed(r->path, "/incr/", 6, &rest))
			sign = 1;
		else if (prefixed(r->path, "/decr/", 6, &rest))
			sign = -1;
		else if (prefixed(r->path, "/append/", 8, &rest))
			prepend = 0;
		else if (prefixed(r->path, "/prepend/", 9, &rest))
			prepend = 1;
		if (sign || prepend >= 0) {
			if (key_or_fail(c, out, ka, rest, &k) < 0)
				return;
			if (sign)
				do_incr(c, r, out, ka, &k, sign);
			else
				do_cat(c, r, out, ka, &k, prepend);
			return;
		}
		if (prefixed(r->path, "/touch/", 7, &rest)) {
			if (key_or_fail(c, out, ka, rest, &k) < 0)
				return;
			do_touch(c, r, out, ka, &k);
			return;
		}
		/* The peer facing endpoint.  It is not part of the client
		 * API and is deliberately not in doc/API.md: it exists for
		 * the flusher on another node, and applying a batch here
		 * never marks anything dirty, which is what stops a write
		 * circulating for ever. */
		if (r->path.n == 7 && !memcmp(r->path.p, "/x/repl", 7)) {
			u32 applied = 0;

			if (!c->cl) {
				fail(c, out, 404, ka, "not in a cluster");
				return;
			}
			if (cl_apply(c->cl, r->body.p, r->body.n,
			             &applied) < 0) {
				fail(c, out, 400, ka, "bad replication batch");
				return;
			}
			/* A replicated write lands underneath this worker's
			 * cached answers just as a local one does. */
			if (applied && c->hot)
				hot_dirty(c->hot);
			http_simple(out, 204, clk_date(), ka, c->minimal_req);
			return;
		}
		if (r->path.n == 5 && !memcmp(r->path.p, "/mget", 5)) {
			do_mget(c, r, out, ka, 0);
			return;
		}
		if (r->path.n == 5 && !memcmp(r->path.p, "/mdel", 5)) {
			do_mget(c, r, out, ka, 1);
			return;
		}
		if (r->path.n == 5 && !memcmp(r->path.p, "/mset", 5)) {
			do_mset(c, r, out, ka);
			return;
		}
	}
	if (r->path.n == 6 && !memcmp(r->path.p, "/flush", 6)) {
		if (r->meth != M_POST && r->meth != M_DELETE) {
			fail(c, out, 405, ka, "method not allowed");
			return;
		}
		if (!c->allow_flush) {
			fail(c, out, 403, ka, "flush is disabled");
			return;
		}
		db_flush(c->db);
		if (c->hot)
			hot_dirty(c->hot);
		http_simple(out, 204, clk_date(), ka, c->minimal_req);
		return;
	}
	if (r->meth == M_GET || r->meth == M_HEAD) {
		if (r->path.n == 6 && !memcmp(r->path.p, "/stats", 6)) {
			do_stats(c, out, ka, 0);
			return;
		}
		if (r->path.n == 8 && !memcmp(r->path.p, "/metrics", 8)) {
			do_stats(c, out, ka, 1);
			return;
		}
		if (r->path.n == 7 && !memcmp(r->path.p, "/health", 7)) {
			reply_text(c, out, 200, ka, "ok", 2);
			return;
		}
		if (r->path.n == 1) {
			reply_text(c, out, 200, ka, index_page,
			           sizeof(index_page) - 1);
			return;
		}
	}
	fail(c, out, 404, ka, "no such endpoint");
}

/* ---- batch operations -------------------------------------------------
 *
 * One request, many keys.  A single GET spends roughly 230ns in the
 * store and 27us getting there and back, so for a caller that needs
 * fifty keys the round trip is the entire cost and the only thing worth
 * removing.  Pipelining removes it too, but almost no HTTP client will
 * pipeline, whereas any of them can post a list.
 *
 * A batch is emphatically not a snapshot.  The keys land in different
 * shards and each is taken under its own lock in turn, so the result is
 * N independent operations that happened to share a request, and another
 * client's write can land in the middle of one.  Redis can promise
 * otherwise because it runs commands on a single thread; buying the same
 * promise here would mean holding several shard locks at once, in a
 * fixed order, for the length of the batch - which would cost far more
 * than it is worth to a cache whose callers already cope with a write
 * landing between two separate GETs. */

/* Splits the body on newlines; a trailing \r is tolerated. */
static int
batch_line(const char **pp, const char *end, Str *line)
{
	const char *p = *pp, *nl;

	if (p >= end)
		return 0;
	nl = memchr(p, '\n', (size_t)(end - p));
	line->p = p;
	line->n = nl ? (size_t)(nl - p) : (size_t)(end - p);
	if (line->n && line->p[line->n - 1] == '\r')
		line->n--;
	*pp = nl ? nl + 1 : end;
	return 1;
}

static int
batch_field(Str *rest, Str *field)
{
	const char *sp;

	while (rest->n && *rest->p == ' ') {
		rest->p++;
		rest->n--;
	}
	if (!rest->n)
		return 0;
	sp = memchr(rest->p, ' ', rest->n);
	field->p = rest->p;
	field->n = sp ? (size_t)(sp - rest->p) : rest->n;
	rest->p += field->n;
	rest->n -= field->n;
	return 1;
}

static void
do_mget(Ctx *c, const Req *r, Buf *out, int ka, int del)
{
	size_t mark;
	const char *p = r->body.p, *end = r->body.p + r->body.n;
	Hdrs h;
	Str line;
	Key k;
	u32 n = 0, found = 0;
	int full = 0;

	/* /mdel comes through here too, and that one writes. */
	if (del && c->hot)
		hot_dirty(c->hot);
	mark = out->len;

	while (batch_line(&p, end, &line)) {
		DbMeta m;
		char pre[24];
		size_t vmark;
		int rc = DB_ENOENT, tries;

		if (!line.n)
			continue;                  /* blank lines are ignored */
		if (++n > CFG_BATCH_MAX) {
			out->len = mark;
			fail(c, out, 413, ka, "too many keys in one batch");
			return;
		}
		if (take_key(line, &k) < 0) {
			out->len = mark;
			fail(c, out, 400, ka, "bad key in batch");
			return;
		}
		if (del) {
			if (db_del(c->db, k.c, k.n, 0, 0) == DB_OK)
				found++;
			continue;
		}
		/* A batch names keys but is answered with values, so the body
		 * a few hundred bytes of key names buy is bounded by nothing
		 * but the store: a thousand keys of the largest size is a
		 * quarter of a gigabyte of response buffered per connection,
		 * from a request that cost the client almost nothing to send.
		 * The container dumps already stop at CFG_CONT_DUMP_MAX for
		 * the same reason; this is the same cap, and what is left is
		 * reported the same way, so a caller can ask for the rest. */
		if (out->len - mark >= CFG_CONT_DUMP_MAX) {
			full = 1;
			n--;
			break;
		}
		vmark = out->len;
		for (tries = 0; tries < 4; tries++) {
			u32 cap;

			if (buf_room(out) < 512)
				buf_grow(out, 512, 0);
			cap = (u32)MIN(buf_room(out), (size_t)0xffffffffu);
			rc = db_get(c->db, k.c, k.n, buf_tail(out), cap, &m);
			if (rc != DB_ESMALL)
				break;
			buf_grow(out, m.vlen, 0);
		}
		if (rc == DB_OK) {
			/* the length is only known once the value is in
			 * place, so the frame header goes in afterwards */
			size_t pn = fmt_u64(pre, m.vlen);

			pre[pn++] = '\n';
			out->len += m.vlen;
			buf_insert(out, vmark, pre, pn);
			buf_putc(out, '\n');
			found++;
		} else {
			buf_puts(out, "-1\n");
		}
	}

	if (del) {
		st_add(&c->st->dels, found);
		hdrs_start(&h, 204, clk_date(), c->minimal_req);
		hdrs_num(&h, "X-Kache-Count", (i64)n);
		hdrs_num(&h, "X-Kache-Deleted", (i64)found);
		hdrs_end(&h, 0, ka);
		buf_put(out, h.b, h.n);
		return;
	}
	st_add(&c->st->hits, found);
	st_add(&c->st->misses, n - found);
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_BIN);
	hdrs_num(&h, "X-Kache-Count", (i64)n);
	hdrs_num(&h, "X-Kache-Hits", (i64)found);
	if (full)
		hdrs_lit(&h, "X-Kache-Truncated", "1");
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
}

typedef struct MRec {
	Key         k;
	const char *v;
	u32         vlen;
	i64         ttl;
} MRec;

/* One record: "<key> <bytes> [<ttl>]\n" then exactly that many raw bytes
 * and an optional newline.  The key is percent encoded as it is in a
 * path, the value is length prefixed and so may contain anything.
 * Returns 1 for a record, 0 at the end of the body, -1 on malformed. */
static int
batch_record(const char **pp, const char *end, i64 dflt, MRec *rec,
             const char **why)
{
	Str line, rest, f;
	const char *p;
	u64 vlen;

	do {
		if (!batch_line(pp, end, &line))
			return 0;
	} while (!line.n);

	rest = line;
	if (!batch_field(&rest, &f)) {
		*why = "missing key";
		return -1;
	}
	if (take_key(f, &rec->k) < 0) {
		*why = "bad key";
		return -1;
	}
	if (!batch_field(&rest, &f)) {
		*why = "missing value length";
		return -1;
	}
	if (parse_u64(f.p, f.n, &vlen) < 0 || vlen > 0xffffffffull) {
		*why = "bad value length";
		return -1;
	}
	rec->ttl = dflt;
	if (batch_field(&rest, &f)) {
		i64 t;

		if (parse_i64(f.p, f.n, &t) < 0 || t < 0 || t > INT64_MAX / 1000) {
			*why = "bad ttl";
			return -1;
		}
		rec->ttl = t ? t * 1000 : DB_FOREVER;
	}
	p = *pp;
	if ((u64)(end - p) < vlen) {
		*why = "value shorter than its declared length";
		return -1;
	}
	rec->v = p;
	rec->vlen = (u32)vlen;
	p += vlen;
	if (p < end && *p == '\r')
		p++;
	if (p < end && *p == '\n')
		p++;
	*pp = p;
	return 1;
}

static void
do_mset(Ctx *c, const Req *r, Buf *out, int ka)
{
	const char *p, *end = r->body.p + r->body.n;
	const char *why = "malformed batch";
	MRec rec;
	Hdrs h;
	i64 ttl;
	u32 flags, n = 0, stored = 0;
	int rc, toomany = 0;

	/* This worker just changed a value, so its own cached answers
	 * are suspect.  Retiring the whole set is one store; finding
	 * the one entry would cost a hash of the written key on every
	 * write, to retire at most one. */
	if (c->hot)
		hot_dirty(c->hot);

	if (take_ttl(r, c->default_ttl, &ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	if (take_flags(r, &flags) < 0) {
		fail(c, out, 400, ka, "bad flags");
		return;
	}

	/* Pass one parses the whole body without touching the store.  A
	 * batch cannot be applied atomically, but it can at least be
	 * rejected atomically, so a malformed one changes nothing. */
	p = r->body.p;
	while ((rc = batch_record(&p, end, ttl, &rec, &why)) == 1) {
		if (++n > CFG_BATCH_MAX) {
			why = "too many records in one batch";
			toomany = 1;
			rc = -1;
			break;
		}
		if (rec.vlen > c->db->map.maxval) {
			why = "value too large";
			rc = -1;
			break;
		}
	}
	if (rc < 0) {
		fail(c, out, toomany ? 413 : 400, ka, why);
		return;
	}

	p = r->body.p;
	while (batch_record(&p, end, ttl, &rec, &why) == 1) {
		if (db_set(c->db, rec.k.c, rec.k.n, rec.v, rec.vlen, rec.ttl,
		           flags, SET_ANY, 0, NULL) == DB_OK)
			stored++;
	}
	st_add(&c->st->sets, stored);
	if (stored != n)
		st_inc(&c->st->errors);

	/* Everything parsed, so the only way a record can fail now is the
	 * arena refusing to make room; say which ones landed. */
	hdrs_start(&h, stored == n ? 204 : 507, clk_date(), c->minimal_req);
	hdrs_num(&h, "X-Kache-Count", (i64)n);
	hdrs_num(&h, "X-Kache-Stored", (i64)stored);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

/* ---- containers -------------------------------------------------------
 *
 * Two shapes of key with a structure inside them, and one rule that
 * makes the interface fall out: the path names the outer key, the query
 * names everything inside it.  A field is `?f=`, an end of a queue is
 * `?side=`, a count is `?n=`.  The alternative - a second path segment -
 * cannot say where an outer key that contains a slash ends and the field
 * begins, and keys here are bytes, not words.
 *
 * Expiry comes in two: `?ttl=` is the field's or the message's, `?kttl=`
 * is the key's.  They are independent, so a session map can hold entries
 * that lapse in a minute inside a key that lapses in a day, and neither
 * has to know about the other.
 *
 * Anything carrying more than one item is framed by length rather than
 * delimited, which is what keeps field names and values binary safe:
 *
 *   map:   "<fieldlen> <valuelen> [ttl]\n" <field> <value> "\n"
 *   queue: "<valuelen> [ttl]\n" <value> "\n"
 *
 * The map frame is exactly what GET /kkv/<key> answers with, so a dump
 * of one map is a legal body for a write to another. */

/* 1 the request named a field, 0 it did not, -1 malformed, -2 too long */
static int
take_field(const Req *r, Key *f)
{
	Str v;
	int rc;

	if (query_get(r->query, "f", &v) < 0)
		return 0;
	if ((rc = take_key(v, f)) < 0)
		return rc;
	return 1;
}

static int
field_or_fail(Ctx *c, Buf *out, int ka, const Req *r, Key *f)
{
	int rc = take_field(r, f);

	if (rc == -2)
		fail(c, out, 414, ka, "field too long");
	else if (rc < 0)
		fail(c, out, 400, ka, "bad field");
	else if (rc == 0)
		fail(c, out, 400, ka, "this endpoint needs ?f=<field>");
	return rc == 1 ? 0 : -1;
}

/* which end of a queue a parameter names; -1 on anything else */
static int
take_side(const Req *r, const char *name, int dflt)
{
	Str v;

	if (query_get(r->query, name, &v) < 0)
		return dflt;
	if (v.n == 1 && (*v.p == 'l' || *v.p == 'L'))
		return Q_LEFT;
	if (v.n == 1 && (*v.p == 'r' || *v.p == 'R'))
		return Q_RIGHT;
	if (v.n == 4 && !memcmp(v.p, "left", 4))
		return Q_LEFT;
	if (v.n == 5 && !memcmp(v.p, "right", 5))
		return Q_RIGHT;
	return -1;
}

/* how many items the request asked for; n absent means one, unframed */
static int
take_count(const Req *r, i64 dflt, i64 cap, i64 *out, int *given)
{
	Str v;

	*given = query_get(r->query, "n", &v) == 0;
	*out = dflt;
	if (!*given)
		return 0;
	if (parse_i64(v.p, v.n, out) < 0 || *out < 0)
		return -1;
	if (*out > cap)
		*out = cap;
	return 0;
}

static void
cont_headers(Hdrs *h, const DbCont *ci)
{
	hdrs_num(h, "X-Kache-Count", (i64)ci->count);
	hdrs_num(h, "X-Kache-Bytes", (i64)ci->bytes);
	hdrs_num(h, "X-Kache-Key-TTL",
	         ci->ttl < 0 ? -1 : (i64)((ci->ttl + 999) / 1000));
	hdrs_num(h, "X-Kache-Key-Version", (i64)ci->version);
}

static i64
ttl_secs(i64 ms)
{
	return ms < 0 ? -1 : (ms + 999) / 1000;
}

/* the conditional headers, read the same way for a key and for a field */
static int
take_mode(Ctx *c, const Req *r, Buf *out, int ka, int *mode, u64 *cas)
{
	*mode = SET_ANY;
	*cas = 0;
	if (r->ifnone.n) {
		if (r->ifnone.n != 1 || r->ifnone.p[0] != '*') {
			fail(c, out, 400, ka,
			     "only If-None-Match: * is supported");
			return -1;
		}
		*mode = SET_ADD;
	} else if (r->ifmatch.n) {
		if (r->ifmatch.n == 1 && r->ifmatch.p[0] == '*') {
			*mode = SET_REPLACE;
		} else if (etag_value(r->ifmatch, cas) < 0) {
			fail(c, out, 400, ka, "malformed If-Match");
			return -1;
		} else {
			*mode = SET_CAS;
		}
	}
	return 0;
}

static int
kkv_opts(Ctx *c, const Req *r, Buf *out, int ka, DbKkvOpt *o)
{
	int has;

	memset(o, 0, sizeof(*o));
	o->mode = SET_ANY;
	if (take_ttl(r, c->default_ttl, &o->ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return -1;
	}
	if ((has = take_kttl(r, c->default_ttl, &o->kttl)) < 0) {
		fail(c, out, 400, ka, "bad kttl");
		return -1;
	}
	/* Only a request that said so moves the key's own expiry.  A write
	 * to one field of a long lived map must not quietly restart the
	 * clock on the map. */
	o->set_kttl = has;
	if (take_flags(r, &o->flags) < 0) {
		fail(c, out, 400, ka, "bad flags");
		return -1;
	}
	return 0;
}

/* ---- one field ------------------------------------------------------- */

static void
kkv_one_get(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, const Key *f)
{
	size_t mark = out->len;
	DbMeta m;
	Hdrs h;
	u8 nothing;
	int rc = DB_OK, tries, head = (r->meth == M_HEAD);

	if (head) {
		rc = db_kkv_get(c->db, k->c, k->n, f->c, f->n, &nothing, 0, &m);
		if (rc == DB_ESMALL)
			rc = DB_OK;
	} else {
		for (tries = 0; tries < 4; tries++) {
			u32 cap;

			if (buf_room(out) < 512)
				buf_grow(out, 512, 0);
			cap = (u32)MIN(buf_room(out), (size_t)0xffffffffu);
			rc = db_kkv_get(c->db, k->c, k->n, f->c, f->n,
			                buf_tail(out), cap, &m);
			if (rc != DB_ESMALL)
				break;
			buf_grow(out, m.vlen, 0);
		}
		if (rc == DB_OK)
			out->len += m.vlen;
	}
	if (rc != DB_OK) {
		st_inc(&c->st->misses);
		if (rc == DB_ENOENT)
			reply_text(c, out, 404, ka, "not found", 9);
		else
			fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->hits);
	st_inc(&c->st->kkv_reads);
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	if (!c->minimal_req || head) {
		hdrs_lit(&h, "Content-Type", CT_BIN);
		meta_headers(&h, &m);
	}
	hdrs_end(&h, m.vlen, ka);
	http_wrap(out, mark, &h);
}

static void
kkv_one_put(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, const Key *f)
{
	DbKkvOpt o;
	DbMeta m;
	DbCont ci;
	Hdrs h;
	int rc;

	if (kkv_opts(c, r, out, ka, &o) < 0)
		return;
	if (take_mode(c, r, out, ka, &o.mode, &o.cas) < 0)
		return;
	memset(&ci, 0, sizeof(ci));
	rc = db_kkv_set(c->db, k->c, k->n, f->c, f->n, r->body.p,
	                (u32)r->body.n, &o, &m, &ci);
	if (rc != DB_OK) {
		if (rc == DB_ENOENT && o.mode != SET_ANY)
			fail(c, out, 412, ka, "precondition failed");
		else
			fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->kkv_writes);
	hdrs_start(&h, m.created ? 201 : 204, clk_date(), c->minimal_req);
	meta_headers(&h, &m);
	cont_headers(&h, &ci);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
kkv_one_del(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, const Key *f)
{
	DbCont ci;
	Hdrs h;
	u64 cas = 0;
	int use_cas = 0, rc;

	if (r->ifmatch.n && !(r->ifmatch.n == 1 && r->ifmatch.p[0] == '*')) {
		if (etag_value(r->ifmatch, &cas) < 0) {
			fail(c, out, 400, ka, "malformed If-Match");
			return;
		}
		use_cas = 1;
	}
	memset(&ci, 0, sizeof(ci));
	rc = db_kkv_del(c->db, k->c, k->n, f->c, f->n, use_cas, cas, &ci);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->kkv_dels);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	cont_headers(&h, &ci);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

/* ---- the whole map --------------------------------------------------- */

typedef struct Dump {
	Buf   *out;
	size_t mark;
	size_t cap;      /* body bytes this response may reach */
	u32    max;      /* items it may carry */
	u32    n;
	int    framed;
	int    full;     /* stopped before the container ran out */
	u64    id;       /* the single unframed item's metadata */
	i64    ttl;
	u32    flags;
} Dump;

static int
dump_fld(void *arg, const void *f, u32 fl, const void *v, u32 vl,
         const DbMeta *m)
{
	Dump *d = arg;
	char pre[80];
	size_t pn;

	if (d->n >= d->max || d->out->len - d->mark >= d->cap) {
		d->full = 1;
		return 1;
	}
	pn = fmt_u64(pre, fl);
	pre[pn++] = ' ';
	pn += fmt_u64(pre + pn, vl);
	pre[pn++] = ' ';
	pn += fmt_i64(pre + pn, ttl_secs(m->ttl));
	pre[pn++] = '\n';
	buf_put(d->out, pre, pn);
	buf_put(d->out, f, fl);
	buf_put(d->out, v, vl);
	buf_putc(d->out, '\n');
	d->n++;
	return 0;
}

static void
kkv_dump(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	size_t mark = out->len;
	DbCont ci;
	Dump d;
	Hdrs h;
	i64 want;
	int rc, given;

	if (take_count(r, CFG_CONT_BATCH_MAX, CFG_CONT_BATCH_MAX, &want,
	               &given) < 0) {
		fail(c, out, 400, ka, "bad n");
		return;
	}
	memset(&ci, 0, sizeof(ci));
	memset(&d, 0, sizeof(d));
	d.out = out;
	d.mark = mark;
	d.cap = CFG_CONT_DUMP_MAX;
	d.max = (u32)want;
	/* ?n=0 asks for the counters alone, which costs one lookup instead
	 * of a walk of the whole map */
	if (want == 0)
		rc = db_kkv_info(c->db, k->c, k->n, &ci);
	else
		rc = db_kkv_scan(c->db, k->c, k->n, dump_fld, &d, &ci);
	if (rc != DB_OK) {
		out->len = mark;
		st_inc(&c->st->misses);
		if (rc == DB_ENOENT)
			reply_text(c, out, 404, ka, "not found", 9);
		else
			fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->hits);
	st_inc(&c->st->kkv_reads);
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_BIN);
	cont_headers(&h, &ci);
	hdrs_num(&h, "X-Kache-Returned", (i64)d.n);
	if (d.full)
		hdrs_lit(&h, "X-Kache-Truncated", "1");
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
	no_body(out, mark, &h, c->head);
}

static void
kkv_drop(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	int rc = db_kkv_drop(c->db, k->c, k->n);

	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->kkv_dels);
	http_simple(out, 204, clk_date(), ka, c->minimal_req);
}

/* "<fieldlen> <valuelen> [ttl]\n" then that many raw bytes of each, and
 * an optional newline.  1 a record, 0 the end of the body, -1 malformed. */
static int
kkv_frame(const char **pp, const char *end, i64 dflt, DbItem *it,
          const char **why)
{
	Str line, rest, f;
	const char *p;
	u64 fl, vl;
	i64 ttl = dflt;

	do {
		if (!batch_line(pp, end, &line))
			return 0;
	} while (!line.n);
	rest = line;
	if (!batch_field(&rest, &f) || parse_u64(f.p, f.n, &fl) < 0 || !fl ||
	    fl > 0xffffffffull) {
		*why = "bad field length";
		return -1;
	}
	if (!batch_field(&rest, &f) || parse_u64(f.p, f.n, &vl) < 0 ||
	    vl > 0xffffffffull) {
		*why = "bad value length";
		return -1;
	}
	if (batch_field(&rest, &f)) {
		i64 t;

		if (parse_i64(f.p, f.n, &t) < 0 || t > INT64_MAX / 1000) {
			*why = "bad ttl";
			return -1;
		}
		/* 0 and -1 both mean no expiry, so what GET /kkv/<key>
		 * answers with is a legal body for a write */
		ttl = t > 0 ? t * 1000 : DB_FOREVER;
	}
	p = *pp;
	if ((u64)(end - p) < fl + vl) {
		*why = "record shorter than its declared lengths";
		return -1;
	}
	if (it) {
		it->k = p;
		it->kl = (u32)fl;
		it->v = p + fl;
		it->vl = (u32)vl;
		it->ttl = ttl;
	}
	p += fl + vl;
	if (p < end && *p == '\r')
		p++;
	if (p < end && *p == '\n')
		p++;
	*pp = p;
	return 1;
}

/* "<len>\n" then that many raw bytes: a bare name, for a batch delete */
static int
name_frame(const char **pp, const char *end, DbItem *it, const char **why)
{
	Str line, rest, f;
	const char *p;
	u64 nl;

	do {
		if (!batch_line(pp, end, &line))
			return 0;
	} while (!line.n);
	rest = line;
	if (!batch_field(&rest, &f) || parse_u64(f.p, f.n, &nl) < 0 || !nl ||
	    nl > 0xffffffffull) {
		*why = "bad field length";
		return -1;
	}
	p = *pp;
	if ((u64)(end - p) < nl) {
		*why = "name shorter than its declared length";
		return -1;
	}
	if (it) {
		it->k = p;
		it->kl = (u32)nl;
		it->v = NULL;
		it->vl = 0;
		it->ttl = DB_FOREVER;
	}
	p += nl;
	if (p < end && *p == '\r')
		p++;
	if (p < end && *p == '\n')
		p++;
	*pp = p;
	return 1;
}

/* Count the records first, so a malformed batch is rejected without
 * touching the store, and so the item array is sized exactly once. */
static int
frame_count(const char *p, const char *end, i64 dflt, int names,
            u32 *out, const char **why, int *toomany)
{
	u32 n = 0;
	int rc;

	*toomany = 0;
	for (;;) {
		rc = names ? name_frame(&p, end, NULL, why)
		           : kkv_frame(&p, end, dflt, NULL, why);
		if (rc != 1)
			break;
		if (++n > CFG_CONT_BATCH_MAX) {
			*why = "too many items in one batch";
			*toomany = 1;
			return -1;
		}
	}
	if (rc < 0)
		return -1;
	*out = n;
	return 0;
}

static void
kkv_mset(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	const char *p, *end = r->body.p + r->body.n;
	const char *why = "malformed batch";
	DbKkvOpt o;
	DbCont ci;
	DbItem stack[16], *it;
	Hdrs h;
	u32 n = 0, i, stored = 0;
	int rc, toomany;

	if (kkv_opts(c, r, out, ka, &o) < 0)
		return;
	if (frame_count(r->body.p, end, o.ttl, 0, &n, &why, &toomany) < 0) {
		fail(c, out, toomany ? 413 : 400, ka, why);
		return;
	}
	it = n <= LEN(stack) ? stack : emalloc((size_t)n * sizeof(*it));
	p = r->body.p;
	for (i = 0; i < n; i++)
		if (kkv_frame(&p, end, o.ttl, &it[i], &why) != 1)
			break;
	memset(&ci, 0, sizeof(ci));
	rc = db_kkv_mset(c->db, k->c, k->n, it, i, &o, &stored, &ci);
	if (it != stack)
		free(it);
	if (rc != DB_OK && rc != DB_ENOSPC) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_add(&c->st->kkv_writes, stored);
	if (stored != n)
		st_inc(&c->st->errors);
	hdrs_start(&h, stored == n ? 204 : 507, clk_date(), c->minimal_req);
	cont_headers(&h, &ci);
	hdrs_num(&h, "X-Kache-Stored", (i64)stored);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
do_kkv_mdel(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	const char *p, *end = r->body.p + r->body.n;
	const char *why = "malformed batch";
	DbCont ci;
	DbItem stack[16], *it;
	Hdrs h;
	u32 n = 0, i, removed = 0;
	int rc, toomany;

	if (frame_count(r->body.p, end, DB_FOREVER, 1, &n, &why, &toomany) < 0) {
		fail(c, out, toomany ? 413 : 400, ka, why);
		return;
	}
	it = n <= LEN(stack) ? stack : emalloc((size_t)n * sizeof(*it));
	p = r->body.p;
	for (i = 0; i < n; i++)
		if (name_frame(&p, end, &it[i], &why) != 1)
			break;
	memset(&ci, 0, sizeof(ci));
	rc = db_kkv_mdel(c->db, k->c, k->n, it, i, &removed, &ci);
	if (it != stack)
		free(it);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_add(&c->st->kkv_dels, removed);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	cont_headers(&h, &ci);
	hdrs_num(&h, "X-Kache-Removed", (i64)removed);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
do_kkv(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	Key f;
	int has = take_field(r, &f);

	if (has == -2) {
		fail(c, out, 414, ka, "field too long");
		return;
	}
	if (has < 0) {
		fail(c, out, 400, ka, "bad field");
		return;
	}
	switch (r->meth) {
	case M_GET:
	case M_HEAD:
		if (has)
			kkv_one_get(c, r, out, ka, k, &f);
		else
			kkv_dump(c, r, out, ka, k);
		return;
	case M_PUT:
	case M_POST:
		if (has)
			kkv_one_put(c, r, out, ka, k, &f);
		else
			kkv_mset(c, r, out, ka, k);
		return;
	case M_DELETE:
		if (has)
			kkv_one_del(c, r, out, ka, k, &f);
		else
			kkv_drop(c, r, out, ka, k);
		return;
	}
	fail(c, out, 405, ka, "method not allowed");
}

static void
do_kkv_incr(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, int sign)
{
	size_t mark = out->len;
	DbKkvOpt o;
	DbMeta m;
	DbCont ci;
	Hdrs h;
	Key f;
	i64 delta = 1, init = 0, result;
	int rc;

	if (field_or_fail(c, out, ka, r, &f) < 0)
		return;
	if (kkv_opts(c, r, out, ka, &o) < 0)
		return;
	if (query_i64(r->query, "by", &delta) == -2) {
		fail(c, out, 400, ka, "bad by");
		return;
	}
	if (query_i64(r->query, "init", &init) == -2) {
		fail(c, out, 400, ka, "bad init");
		return;
	}
	if (sign < 0) {
		if (delta == INT64_MIN) {
			fail(c, out, 409, ka, "delta out of range");
			return;
		}
		delta = -delta;
	}
	/* no ttl of its own means the field keeps the one it has */
	{
		i64 unused;

		if (take_ttl(r, c->default_ttl, &unused) == 0)
			o.mode |= SET_KEEPTTL;
	}
	memset(&ci, 0, sizeof(ci));
	rc = db_kkv_incr(c->db, k->c, k->n, f.c, f.n, delta, init, &o,
	                 &result, &m, &ci);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->incrs);
	st_inc(&c->st->kkv_writes);
	buf_puti(out, result);
	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_TXT);
	meta_headers(&h, &m);
	cont_headers(&h, &ci);
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
}

static void
do_kkv_touch(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	DbMeta m;
	DbCont ci;
	Hdrs h;
	Key f;
	i64 ttl;
	int has = take_field(r, &f), rc;

	if (has == -2) {
		fail(c, out, 414, ka, "field too long");
		return;
	}
	if (has < 0) {
		fail(c, out, 400, ka, "bad field");
		return;
	}
	if (take_ttl(r, c->default_ttl, &ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	memset(&m, 0, sizeof(m));
	memset(&ci, 0, sizeof(ci));
	rc = db_kkv_touch(c->db, k->c, k->n, has ? f.c : NULL, has ? f.n : 0,
	                  ttl, &m, &ci);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->touches);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	if (has)
		meta_headers(&h, &m);
	cont_headers(&h, &ci);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

/* ---- queues ----------------------------------------------------------- */

static int
q_opts(Ctx *c, const Req *r, Buf *out, int ka, DbQOpt *o, int dfl_side)
{
	i64 v;
	int rc;

	memset(o, 0, sizeof(*o));
	if ((rc = take_ttl(r, c->default_ttl, &o->ttl)) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return -1;
	}
	o->set_ttl = rc;
	if ((rc = take_kttl(r, c->default_ttl, &o->qttl)) < 0) {
		fail(c, out, 400, ka, "bad kttl");
		return -1;
	}
	o->set_qttl = rc;
	if (take_flags(r, &o->flags) < 0) {
		fail(c, out, 400, ka, "bad flags");
		return -1;
	}
	rc = query_i64(r->query, "maxlen", &v);
	if (rc == -2 || (rc == 0 && v < 0)) {
		fail(c, out, 400, ka, "bad maxlen");
		return -1;
	}
	o->maxlen = rc == 0 ? (u64)v : 0;
	if ((o->right = take_side(r, "side", dfl_side)) < 0) {
		fail(c, out, 400, ka, "side must be l or r");
		return -1;
	}
	return 0;
}

/* One entry out.  A request that asked for a count gets frames; one that
 * did not asked for a single message and gets its bytes, with the
 * metadata in headers - which is what a worker taking one job wants, and
 * what curl can use without a parser. */
static int
dump_ent(void *arg, const void *v, u32 vl, const DbQMeta *e)
{
	Dump *d = arg;
	char pre[80];
	size_t pn;

	if (d->n >= d->max || d->out->len - d->mark >= d->cap) {
		d->full = 1;
		return 1;
	}
	if (d->framed) {
		pn = fmt_u64(pre, vl);
		pre[pn++] = ' ';
		pn += fmt_i64(pre + pn, ttl_secs(e->ttl));
		pre[pn++] = ' ';
		pn += fmt_u64(pre + pn, e->id);
		pre[pn++] = '\n';
		buf_put(d->out, pre, pn);
		buf_put(d->out, v, vl);
		buf_putc(d->out, '\n');
	} else {
		buf_put(d->out, v, vl);
		d->id = e->id;
		d->ttl = e->ttl;
		d->flags = e->flags;
	}
	d->n++;
	return 0;
}

static void
q_out(Ctx *c, Buf *out, int ka, size_t mark, const Dump *d, const DbCont *ci)
{
	Hdrs h;

	hdrs_start(&h, 200, clk_date(), c->minimal_req);
	hdrs_lit(&h, "Content-Type", CT_BIN);
	cont_headers(&h, ci);
	hdrs_num(&h, "X-Kache-Returned", (i64)d->n);
	if (!d->framed) {
		hdrs_num(&h, "X-Kache-Id", (i64)d->id);
		hdrs_num(&h, "X-Kache-TTL", ttl_secs(d->ttl));
		hdrs_num(&h, "X-Kache-Flags", (i64)d->flags);
	}
	if (d->full)
		hdrs_lit(&h, "X-Kache-Truncated", "1");
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
	no_body(out, mark, &h, c->head);
}

/* peek and pop differ in one flag and in which counter they bump */
static void
q_read(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, int take)
{
	size_t mark = out->len;
	DbQOpt o;
	DbCont ci;
	Dump d;
	i64 want;
	u32 got = 0;
	int rc, given;

	if (q_opts(c, r, out, ka, &o, Q_LEFT) < 0)
		return;
	if (take_count(r, 1, CFG_QPOP_MAX, &want, &given) < 0) {
		fail(c, out, 400, ka, "bad n");
		return;
	}
	memset(&ci, 0, sizeof(ci));
	memset(&d, 0, sizeof(d));
	d.out = out;
	d.mark = mark;
	d.cap = CFG_CONT_DUMP_MAX;
	d.max = (u32)want;
	d.framed = given;
	if (take)
		rc = db_q_pop(c->db, k->c, k->n, (u32)want, &o, dump_ent, &d,
		              &got, &ci);
	else
		rc = db_q_peek(c->db, k->c, k->n, (u32)want, &o, dump_ent, &d,
		               &got, &ci);
	if (rc == DB_ENOENT || (rc == DB_OK && d.n == 0)) {
		/* A queue that is empty and a queue that was never there
		 * answer alike: there is nothing to hand over either way,
		 * and a drained queue stops existing. */
		out->len = mark;
		st_inc(&c->st->misses);
		http_simple(out, 204, clk_date(), ka, c->minimal_req);
		return;
	}
	if (rc != DB_OK) {
		out->len = mark;
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->hits);
	if (take)
		st_add(&c->st->q_pops, d.n);
	q_out(c, out, ka, mark, &d, &ci);
}

static void
do_q_pop(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	q_read(c, r, out, ka, k, 1);
}

/* "<valuelen> [ttl]\n" then that many raw bytes and an optional newline */
static int
q_frame(const char **pp, const char *end, i64 dflt, DbItem *it,
        const char **why)
{
	Str line, rest, f;
	const char *p;
	u64 vl;
	i64 ttl = dflt;

	do {
		if (!batch_line(pp, end, &line))
			return 0;
	} while (!line.n);
	rest = line;
	if (!batch_field(&rest, &f) || parse_u64(f.p, f.n, &vl) < 0 ||
	    vl > 0xffffffffull) {
		*why = "bad value length";
		return -1;
	}
	if (batch_field(&rest, &f)) {
		i64 t;

		if (parse_i64(f.p, f.n, &t) < 0 || t > INT64_MAX / 1000) {
			*why = "bad ttl";
			return -1;
		}
		ttl = t > 0 ? t * 1000 : DB_FOREVER;
	}
	p = *pp;
	if ((u64)(end - p) < vl) {
		*why = "message shorter than its declared length";
		return -1;
	}
	if (it) {
		it->k = NULL;
		it->kl = 0;
		it->v = p;
		it->vl = (u32)vl;
		it->ttl = ttl;
	}
	p += vl;
	if (p < end && *p == '\r')
		p++;
	if (p < end && *p == '\n')
		p++;
	*pp = p;
	return 1;
}

static void
q_pushed(Ctx *c, Buf *out, int ka, const DbCont *ci, u32 stored, u32 n)
{
	Hdrs h;

	st_add(&c->st->q_pushes, stored);
	if (stored != n)
		st_inc(&c->st->errors);
	hdrs_start(&h, stored == n ? (ci->created ? 201 : 204) : 507,
	           clk_date(), c->minimal_req);
	cont_headers(&h, ci);
	hdrs_num(&h, "X-Kache-Stored", (i64)stored);
	hdrs_num(&h, "X-Kache-Id", (i64)ci->seq);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
q_push_one(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	DbQOpt o;
	DbCont ci;
	DbItem it;
	u32 stored = 0;
	int rc;

	if (q_opts(c, r, out, ka, &o, Q_RIGHT) < 0)
		return;
	it.k = NULL;
	it.kl = 0;
	it.v = r->body.p;
	it.vl = (u32)r->body.n;
	it.ttl = o.ttl;
	memset(&ci, 0, sizeof(ci));
	rc = db_q_push(c->db, k->c, k->n, &it, 1, &o, &stored, &ci);
	if (rc != DB_OK && rc != DB_ENOSPC) {
		fail_db(c, out, rc, ka);
		return;
	}
	q_pushed(c, out, ka, &ci, stored, 1);
}

static void
do_q_push(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	const char *p, *end = r->body.p + r->body.n;
	const char *why = "malformed batch";
	DbQOpt o;
	DbCont ci;
	DbItem stack[16], *it;
	u32 n = 0, i, stored = 0;
	int rc;

	if (q_opts(c, r, out, ka, &o, Q_RIGHT) < 0)
		return;
	p = r->body.p;
	for (;;) {
		rc = q_frame(&p, end, o.ttl, NULL, &why);
		if (rc != 1)
			break;
		if (++n > CFG_CONT_BATCH_MAX) {
			why = "too many messages in one batch";
			rc = -1;
			break;
		}
	}
	if (rc < 0) {
		fail(c, out, n > CFG_CONT_BATCH_MAX ? 413 : 400, ka, why);
		return;
	}
	it = n <= LEN(stack) ? stack : emalloc((size_t)n * sizeof(*it));
	p = r->body.p;
	for (i = 0; i < n; i++)
		if (q_frame(&p, end, o.ttl, &it[i], &why) != 1)
			break;
	memset(&ci, 0, sizeof(ci));
	rc = db_q_push(c->db, k->c, k->n, it, i, &o, &stored, &ci);
	if (it != stack)
		free(it);
	if (rc != DB_OK && rc != DB_ENOSPC) {
		fail_db(c, out, rc, ka);
		return;
	}
	q_pushed(c, out, ka, &ci, stored, n);
}

static void
q_drop(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	int rc = db_q_drop(c->db, k->c, k->n);

	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->dels);
	http_simple(out, 204, clk_date(), ka, c->minimal_req);
}

static void
do_q(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	switch (r->meth) {
	case M_GET:
	case M_HEAD:   q_read(c, r, out, ka, k, 0); return;
	case M_PUT:
	case M_POST:   q_push_one(c, r, out, ka, k); return;
	case M_DELETE: q_drop(c, r, out, ka, k); return;
	}
	fail(c, out, 405, ka, "method not allowed");
}

static void
do_q_move(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	size_t mark = out->len;
	DbQOpt o;
	DbCont ci;
	Dump d;
	Key dst;
	Str v;
	int from, to, rc;

	if (q_opts(c, r, out, ka, &o, Q_LEFT) < 0)
		return;
	if (query_get(r->query, "dst", &v) < 0) {
		fail(c, out, 400, ka, "qmove needs ?dst=<key>");
		return;
	}
	if ((rc = take_key(v, &dst)) < 0) {
		fail(c, out, rc == -2 ? 414 : 400, ka, "bad destination key");
		return;
	}
	if ((from = take_side(r, "from", Q_LEFT)) < 0 ||
	    (to = take_side(r, "to", Q_RIGHT)) < 0) {
		fail(c, out, 400, ka, "from and to must be l or r");
		return;
	}
	memset(&ci, 0, sizeof(ci));
	memset(&d, 0, sizeof(d));
	d.out = out;
	d.mark = mark;
	d.cap = CFG_CONT_DUMP_MAX;
	d.max = 1;
	rc = db_q_move(c->db, k->c, k->n, dst.c, dst.n, from, to, &o,
	               dump_ent, &d, &ci);
	if (rc == DB_ENOENT) {
		out->len = mark;
		st_inc(&c->st->misses);
		http_simple(out, 204, clk_date(), ka, c->minimal_req);
		return;
	}
	if (rc != DB_OK) {
		out->len = mark;
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->q_pops);
	st_inc(&c->st->q_pushes);
	q_out(c, out, ka, mark, &d, &ci);
}

static void
do_q_trim(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	DbCont ci;
	Hdrs h;
	i64 maxlen;
	u64 removed = 0;
	int side, rc;

	if (query_i64(r->query, "maxlen", &maxlen) != 0 || maxlen < 0) {
		fail(c, out, 400, ka, "qtrim needs ?maxlen=<n>");
		return;
	}
	if ((side = take_side(r, "side", Q_LEFT)) < 0) {
		fail(c, out, 400, ka, "side must be l or r");
		return;
	}
	memset(&ci, 0, sizeof(ci));
	rc = db_q_trim(c->db, k->c, k->n, (u64)maxlen, side, &removed, &ci);
	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_add(&c->st->dels, removed);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	cont_headers(&h, &ci);
	hdrs_num(&h, "X-Kache-Removed", (i64)removed);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
do_q_touch(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	DbCont ci;
	Hdrs h;
	i64 ttl;
	int rc;

	if (take_ttl(r, c->default_ttl, &ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	memset(&ci, 0, sizeof(ci));
	if ((rc = db_q_touch(c->db, k->c, k->n, ttl, &ci)) != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->touches);
	hdrs_start(&h, 204, clk_date(), c->minimal_req);
	cont_headers(&h, &ci);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}
