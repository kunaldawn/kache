/* kache - just enough HTTP/1.1
 *
 * The parser never copies and never allocates: a Str points into the
 * connection's read buffer.  Only the handful of headers the cache acts
 * on are recognised, the rest are skipped. */
#ifndef KACHE_HTTP_H
#define KACHE_HTTP_H

#include "http/buf.h"
#include "util/util.h"

enum {
	M_NONE = 0,
	M_GET,
	M_HEAD,
	M_PUT,
	M_POST,
	M_DELETE
};

typedef struct Str {
	const char *p;
	size_t      n;
} Str;

typedef struct Req {
	int    meth;
	int    minor;          /* HTTP/1.<minor> */
	Str    path;           /* still percent encoded, no query */
	Str    query;
	Str    body;
	Str    ifmatch;
	Str    ifnone;
	Str    xttl;
	Str    xflags;
	u64    clen;
	size_t hdrlen;
	u8     keepalive;
	u8     expect100;
} Req;

/* >0 header byte count, 0 incomplete, <0 the HTTP status to answer with */
int http_head(const char *buf, size_t len, Req *r);

int  url_decode(const char *s, size_t n, char *out, size_t cap);
int  query_get(Str q, const char *name, Str *val);
int  query_i64(Str q, const char *name, i64 *out);
int  query_u32(Str q, const char *name, u32 *out);
/* strips the quotes and any W/ prefix, then reads the number */
int  etag_value(Str s, u64 *out);

const char *http_status(int code);

/* Response headers are built on the stack, then prepended to a body that
 * is already sitting in the output buffer.  Minimal mode drops the
 * headers a cache client rarely reads back; the flag rides in the Hdrs
 * so hdrs_end does not have to be told a second time. */
#define HDRS_MAX 512

typedef struct Hdrs {
	char   b[HDRS_MAX];
	size_t n;
	int    status;
	int    minimal;
} Hdrs;

void hdrs_start(Hdrs *h, int status, const char *date, int minimal);
void hdrs_add(Hdrs *h, const char *name, const char *val, size_t n);
#define hdrs_lit(h, name, val) hdrs_add((h), (name), (val), sizeof(val) - 1)
void hdrs_num(Hdrs *h, const char *name, i64 v);
void hdrs_etag(Hdrs *h, u64 version);
void hdrs_end(Hdrs *h, size_t clen, int keepalive);

/* insert the finished headers in front of the body appended at mark */
void http_wrap(Buf *out, size_t mark, const Hdrs *h);
/* a complete response with no body of its own */
void http_simple(Buf *out, int status, const char *date, int keepalive,
                 int minimal);

#endif /* KACHE_HTTP_H */
