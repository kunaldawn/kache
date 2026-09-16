/* kache - the mapping from request to store operation, and back to a
 * status code.  This is the only file that knows the HTTP contract. */
#include <string.h>

#include "config.h"
#include "http/route.h"
#include "util/clk.h"

#define KEYMAX CFG_MAX_KEY

#define CT_BIN  "application/octet-stream"
#define CT_TXT  "text/plain; charset=utf-8"

typedef struct Key {
	char c[KEYMAX + 1];
	u32  n;
} Key;

/* ---- small response helpers ----------------------------------------- */

static void
reply_text(Buf *out, int status, int ka, const char *msg, size_t n)
{
	size_t mark = out->len;
	Hdrs h;

	buf_put(out, msg, n);
	buf_putc(out, '\n');
	hdrs_start(&h, status, clk_date());
	hdrs_lit(&h, "Content-Type", CT_TXT);
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
}

static void
fail(Ctx *c, Buf *out, int status, int ka, const char *msg)
{
	st_inc(&c->st->errors);
	reply_text(out, status, ka, msg, strlen(msg));
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
	if ((n = url_decode(rest.p, rest.n, k->c, sizeof(k->c) - 1)) < 0)
		return rest.n > KEYMAX ? -2 : -1;
	if (n == 0)
		return -1;
	k->c[n] = '\0';
	k->n = (u32)n;
	return 0;
}

/* ttl in seconds from the query or the header, 0 meaning "no expiry".
 * Returns 1 when the request carried one, 0 when it did not, -1 on junk. */
static int
take_ttl(const Req *r, i64 dfl, i64 *out)
{
	i64 v;
	int rc;

	if ((rc = query_i64(r->query, "ttlms", &v)) == 0) {
		if (v < 0)
			return -1;
		*out = v ? v : DB_FOREVER;
		return 1;
	}
	if (rc == -2)
		return -1;
	if ((rc = query_i64(r->query, "ttl", &v)) == 0) {
		if (v < 0 || v > INT64_MAX / 1000)
			return -1;
		*out = v ? v * 1000 : DB_FOREVER;
		return 1;
	}
	if (rc == -2)
		return -1;
	if (r->xttl.n) {
		if (parse_i64(r->xttl.p, r->xttl.n, &v) < 0 || v < 0 ||
		    v > INT64_MAX / 1000)
			return -1;
		*out = v ? v * 1000 : DB_FOREVER;
		return 1;
	}
	*out = dfl;
	return 0;
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

static void
do_get(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	size_t mark = out->len;
	DbMeta m;
	Hdrs h;
	u8 nothing;
	int rc, tries, head = (r->meth == M_HEAD);

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
			reply_text(out, 404, ka, "not found", 9);
		else
			fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->hits);
	hdrs_start(&h, 200, clk_date());
	hdrs_lit(&h, "Content-Type", CT_BIN);
	meta_headers(&h, &m);
	hdrs_end(&h, m.vlen, ka);
	http_wrap(out, mark, &h);
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
	hdrs_start(&h, m.created ? 201 : 204, clk_date());
	meta_headers(&h, &m);
	hdrs_end(&h, 0, ka);
	buf_put(out, h.b, h.n);
}

static void
do_del(Ctx *c, const Req *r, Buf *out, int ka, const Key *k)
{
	u64 cas = 0;
	int use_cas = 0, rc;

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
	http_simple(out, 204, clk_date(), ka);
}

static void
do_incr(Ctx *c, const Req *r, Buf *out, int ka, const Key *k, int sign)
{
	size_t mark = out->len;
	DbMeta m;
	Hdrs h;
	i64 delta = 1, init = 0, ttl, result;
	int set_ttl, rc;

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
	buf_puti(out, result);
	hdrs_start(&h, 200, clk_date());
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
	int rc = db_cat(c->db, k->c, k->n, r->body.p, (u32)r->body.n,
	                prepend, &m);

	if (rc != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->cats);
	hdrs_start(&h, 204, clk_date());
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

	if (take_ttl(r, c->default_ttl, &ttl) < 0) {
		fail(c, out, 400, ka, "bad ttl");
		return;
	}
	if ((rc = db_touch(c->db, k->c, k->n, ttl, &m)) != DB_OK) {
		fail_db(c, out, rc, ka);
		return;
	}
	st_inc(&c->st->touches);
	hdrs_start(&h, 204, clk_date());
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
	       ACCEPT, CLOSE, CUR, BIN, BOUT };
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
#undef M
	return n;
}

static void
do_stats(Ctx *c, Buf *out, int ka, int prom)
{
	Metric mt[32];
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
	hdrs_start(&h, 200, clk_date());
	hdrs_lit(&h, "Content-Type", CT_TXT);
	hdrs_end(&h, out->len - mark, ka);
	http_wrap(out, mark, &h);
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
"  POST   /flush               drop everything\n"
"  GET    /stats               counters, one per line\n"
"  GET    /metrics             the same in prometheus form\n"
"  GET    /health              liveness\n"
"\n"
"  ?ttl=<seconds>              0 means never expire\n"
"  ?ttlms=<milliseconds>       finer grained ttl\n"
"  ?flags=<u32>                opaque, returned in X-Kache-Flags\n"
"  If-None-Match: *            store only if absent\n"
"  If-Match: \"<etag>\"          store or delete only on that version\n";

/* ---- dispatch -------------------------------------------------------- */

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
	if (r->meth == M_POST) {
		int sign = 0, prepend = -1;

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
		http_simple(out, 204, clk_date(), ka);
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
			reply_text(out, 200, ka, "ok", 2);
			return;
		}
		if (r->path.n == 1) {
			reply_text(out, 200, ka, index_page,
			           sizeof(index_page) - 1);
			return;
		}
	}
	fail(c, out, 404, ka, "no such endpoint");
}
