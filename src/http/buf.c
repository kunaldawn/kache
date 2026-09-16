/* kache - the growable byte buffer behind every connection and response. */
#include <stdlib.h>

#include "config.h"
#include "http/buf.h"

void
buf_free(Buf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

int
buf_grow(Buf *b, size_t n, size_t max)
{
	size_t want = b->len + n, cap;

	if (want <= b->cap)
		return 0;
	if (max) {
		if (b->len >= max)
			return -1;
		if (want > max)
			want = max;     /* give what is left, not nothing */
		if (want <= b->cap)
			return 0;
	}
	cap = b->cap ? b->cap : CFG_BUF_INIT;
	while (cap < want)
		cap <<= 1;
	if (max && cap > max)
		cap = max;
	b->p = erealloc(b->p, cap);
	b->cap = cap;
	return 0;
}

void
buf_drain(Buf *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->p, b->p + n, b->len - n);
	b->len -= n;
}

void
buf_trim(Buf *b, size_t keep)
{
	if (b->len == 0 && b->cap > keep) {
		free(b->p);
		b->p = NULL;
		b->cap = 0;
	}
}

void
buf_put(Buf *b, const void *p, size_t n)
{
	buf_grow(b, n, 0);
	memcpy(b->p + b->len, p, n);
	b->len += n;
}

void
buf_putc(Buf *b, char c)
{
	buf_grow(b, 1, 0);
	b->p[b->len++] = (u8)c;
}

void
buf_puts(Buf *b, const char *s)
{
	buf_put(b, s, strlen(s));
}

void
buf_putu(Buf *b, u64 v)
{
	buf_grow(b, 20, 0);
	b->len += fmt_u64((char *)b->p + b->len, v);
}

void
buf_puti(Buf *b, i64 v)
{
	buf_grow(b, 21, 0);
	b->len += fmt_i64((char *)b->p + b->len, v);
}
