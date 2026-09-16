/* kache - the HTTP/1.1 request parser and the response builder. */
#include <string.h>

#include "config.h"
#include "http/http.h"
#include "util/clk.h"

#define MAX_LINE 8192
#define MAX_HDRS 64

static inline int
lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int
ieq(const char *a, size_t n, const char *lit)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (!lit[i] || lower((u8)a[i]) != lit[i])
			return 0;
	}
	return lit[n] == '\0';
}

static void
trim(Str *s)
{
	while (s->n && (*s->p == ' ' || *s->p == '\t')) {
		s->p++;
		s->n--;
	}
	while (s->n && (s->p[s->n - 1] == ' ' || s->p[s->n - 1] == '\t'))
		s->n--;
}

static int
method_of(const char *p, size_t n)
{
	if (n == 3 && !memcmp(p, "GET", 3))
		return M_GET;
	if (n == 3 && !memcmp(p, "PUT", 3))
		return M_PUT;
	if (n == 4 && !memcmp(p, "HEAD", 4))
		return M_HEAD;
	if (n == 4 && !memcmp(p, "POST", 4))
		return M_POST;
	if (n == 6 && !memcmp(p, "DELETE", 6))
		return M_DELETE;
	return M_NONE;
}

/* Returns the line length excluding the terminator, or -1 when the
 * buffer does not hold a full line yet.  *adv is the bytes to skip. */
static long
line_at(const char *p, size_t len, size_t *adv)
{
	const char *nl = memchr(p, '\n', len);
	size_t n;

	if (!nl)
		return -1;
	n = (size_t)(nl - p);
	*adv = n + 1;
	if (n && p[n - 1] == '\r')
		n--;
	return (long)n;
}

int
http_head(const char *buf, size_t len, Req *r)
{
	const char *p = buf;
	size_t left = len, adv;
	long n;
	int nhdr = 0;
	const char *sp1, *sp2, *q;

	memset(r, 0, sizeof(*r));
	r->clen = 0;

	if ((n = line_at(p, left, &adv)) < 0)
		return len > MAX_LINE ? -414 : 0;
	if (n == 0 || n > MAX_LINE)
		return -400;

	if (!(sp1 = memchr(p, ' ', (size_t)n)))
		return -400;
	if (!(r->meth = method_of(p, (size_t)(sp1 - p))))
		return -405;
	sp1++;
	if (!(sp2 = memchr(sp1, ' ', (size_t)n - (size_t)(sp1 - p))))
		return -400;

	r->path.p = sp1;
	r->path.n = (size_t)(sp2 - sp1);
	if (!r->path.n || r->path.p[0] != '/')
		return -400;
	if ((q = memchr(r->path.p, '?', r->path.n)) != NULL) {
		r->query.p = q + 1;
		r->query.n = r->path.n - (size_t)(q - r->path.p) - 1;
		r->path.n = (size_t)(q - r->path.p);
	}

	sp2++;
	{
		size_t vn = (size_t)n - (size_t)(sp2 - p);

		if (vn < 8 || memcmp(sp2, "HTTP/1.", 7))
			return -505;
		r->minor = sp2[7] - '0';
		if (r->minor != 0 && r->minor != 1)
			return -505;
	}
	r->keepalive = (r->minor == 1);

	p += adv;
	left -= adv;
	for (;;) {
		const char *colon;
		Str name, val;

		if ((n = line_at(p, left, &adv)) < 0)
			return left > MAX_LINE ? -431 : 0;
		if (n == 0) {
			p += adv;
			r->hdrlen = (size_t)(p - buf);
			return (int)r->hdrlen;
		}
		if (++nhdr > MAX_HDRS)
			return -431;
		if (!(colon = memchr(p, ':', (size_t)n)))
			return -400;
		name.p = p;
		name.n = (size_t)(colon - p);
		val.p = colon + 1;
		val.n = (size_t)n - name.n - 1;
		trim(&val);

		switch (lower((u8)name.p[0])) {
		case 'c':
			if (ieq(name.p, name.n, "content-length")) {
				if (parse_u64(val.p, val.n, &r->clen) < 0)
					return -400;
			} else if (ieq(name.p, name.n, "connection")) {
				if (ieq(val.p, val.n, "close"))
					r->keepalive = 0;
				else if (ieq(val.p, val.n, "keep-alive"))
					r->keepalive = 1;
			}
			break;
		case 'e':
			if (ieq(name.p, name.n, "expect") &&
			    ieq(val.p, val.n, "100-continue"))
				r->expect100 = 1;
			break;
		case 'i':
			if (ieq(name.p, name.n, "if-match"))
				r->ifmatch = val;
			else if (ieq(name.p, name.n, "if-none-match"))
				r->ifnone = val;
			break;
		case 't':
			if (ieq(name.p, name.n, "transfer-encoding"))
				return -501;
			break;
		case 'x':
			if (ieq(name.p, name.n, "x-kache-ttl"))
				r->xttl = val;
			else if (ieq(name.p, name.n, "x-kache-flags"))
				r->xflags = val;
			break;
		}
		p += adv;
		left -= adv;
	}
}

static inline int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = lower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

int
url_decode(const char *s, size_t n, char *out, size_t cap)
{
	size_t i, o = 0;

	for (i = 0; i < n; i++) {
		int c = (u8)s[i];

		if (c == '%') {
			int hi, lo;

			if (i + 2 >= n)
				return -1;
			if ((hi = hexval((u8)s[i + 1])) < 0 ||
			    (lo = hexval((u8)s[i + 2])) < 0)
				return -1;
			c = (hi << 4) | lo;
			i += 2;
		}
		if (o >= cap)
			return -1;
		out[o++] = (char)c;
	}
	return (int)o;
}

int
query_get(Str q, const char *name, Str *val)
{
	size_t nl = strlen(name);
	const char *p = q.p, *end = q.p + q.n;

	while (p < end) {
		const char *amp = memchr(p, '&', (size_t)(end - p));
		const char *stop = amp ? amp : end;
		const char *eq = memchr(p, '=', (size_t)(stop - p));

		if (eq && (size_t)(eq - p) == nl && !memcmp(p, name, nl)) {
			val->p = eq + 1;
			val->n = (size_t)(stop - eq - 1);
			return 0;
		}
		if (!amp)
			break;
		p = amp + 1;
	}
	return -1;
}

int
query_i64(Str q, const char *name, i64 *out)
{
	Str v;

	if (query_get(q, name, &v) < 0)
		return -1;
	return parse_i64(v.p, v.n, out) < 0 ? -2 : 0;
}

int
query_u32(Str q, const char *name, u32 *out)
{
	Str v;
	u64 u;

	if (query_get(q, name, &v) < 0)
		return -1;
	if (parse_u64(v.p, v.n, &u) < 0 || u > 0xffffffffull)
		return -2;
	*out = (u32)u;
	return 0;
}

int
etag_value(Str s, u64 *out)
{
	if (s.n >= 2 && s.p[0] == 'W' && s.p[1] == '/') {
		s.p += 2;
		s.n -= 2;
	}
	if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') {
		s.p++;
		s.n -= 2;
	}
	return parse_u64(s.p, s.n, out);
}

const char *
http_status(int code)
{
	switch (code) {
	case 100: return "Continue";
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 412: return "Precondition Failed";
	case 413: return "Payload Too Large";
	case 414: return "URI Too Long";
	case 431: return "Request Header Fields Too Large";
	case 403: return "Forbidden";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 505: return "HTTP Version Not Supported";
	case 507: return "Insufficient Storage";
	}
	return "Error";
}

/* ---- responses ------------------------------------------------------ */

static void
raw(Hdrs *h, const void *p, size_t n)
{
	if (h->n + n > HDRS_MAX)
		return;                       /* bounded by construction */
	memcpy(h->b + h->n, p, n);
	h->n += n;
}

static void
lit(Hdrs *h, const char *s)
{
	raw(h, s, strlen(s));
}

void
hdrs_start(Hdrs *h, int status, const char *date)
{
	const char *txt = http_status(status);

	h->n = 0;
	h->status = status;
	lit(h, "HTTP/1.1 ");
	h->n += fmt_u64(h->b + h->n, (u64)status);
	raw(h, " ", 1);
	lit(h, txt);
	lit(h, "\r\nServer: kache\r\nDate: ");
	raw(h, date, CLK_DATE_LEN);
	lit(h, "\r\n");
}

void
hdrs_add(Hdrs *h, const char *name, const char *val, size_t n)
{
	lit(h, name);
	lit(h, ": ");
	raw(h, val, n);
	lit(h, "\r\n");
}

void
hdrs_num(Hdrs *h, const char *name, i64 v)
{
	lit(h, name);
	lit(h, ": ");
	if (h->n + 21 <= HDRS_MAX)
		h->n += fmt_i64(h->b + h->n, v);
	lit(h, "\r\n");
}

void
hdrs_etag(Hdrs *h, u64 version)
{
	lit(h, "ETag: \"");
	if (h->n + 20 <= HDRS_MAX)
		h->n += fmt_u64(h->b + h->n, version);
	lit(h, "\"\r\n");
}

void
hdrs_end(Hdrs *h, size_t clen, int keepalive)
{
	/* a 204 carries no message body, not even a zero length one */
	if (h->status != 204)
		hdrs_num(h, "Content-Length", (i64)clen);
	lit(h, keepalive ? "Connection: keep-alive\r\n\r\n"
	                 : "Connection: close\r\n\r\n");
}

void
http_wrap(Buf *out, size_t mark, const Hdrs *h)
{
	size_t blen = out->len - mark;

	buf_grow(out, h->n, 0);
	if (blen)
		memmove(out->p + mark + h->n, out->p + mark, blen);
	memcpy(out->p + mark, h->b, h->n);
	out->len += h->n;
}

void
http_simple(Buf *out, int status, const char *date, int keepalive)
{
	Hdrs h;

	hdrs_start(&h, status, date);
	hdrs_end(&h, 0, keepalive);
	buf_put(out, h.b, h.n);
}
