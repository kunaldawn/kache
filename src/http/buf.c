/* kache - the growable byte buffer behind every connection and response. */
#include <stdlib.h>

#include "config.h"
#include "http/buf.h"

void
buf_free(Buf *b)
{
	free(b->p);
	b->p = NULL;
	b->off = b->len = b->cap = 0;
}

int
buf_grow(Buf *b, size_t n, size_t max)
{
	size_t want = b->len + n, cap;
	int shorted = 0;

	/* b->len + n is bounded by the request limits everywhere it is
	 * called from, but the doubling below is what would turn an
	 * overflow into a buffer smaller than the caller is about to write
	 * into, so it is checked here rather than argued about there. */
	if (want < b->len)
		return -1;
	if (want <= b->cap)
		return 0;
	if (max) {
		if (b->len >= max)
			return -1;
		if (want > max) {
			want = max;     /* give what is left, not nothing */
			shorted = 1;
		}
	}
	cap = b->cap ? b->cap : CFG_BUF_INIT;
	while (cap < want) {
		if (cap > (size_t)-1 / 2) {
			cap = want;
			break;
		}
		cap <<= 1;
	}
	if (max && cap > max)
		cap = max;
	if (cap > b->cap) {
		b->p = erealloc(b->p, cap);
		b->cap = cap;
	}
	/* Room was made, but less than was asked for.  Saying so is the
	 * whole difference between a caller that checks buf_room() and one
	 * that writes n bytes into what it was told it had. */
	return shorted ? -1 : 0;
}

void
buf_drain(Buf *b, size_t n)
{
	b->off += n;
	if (b->off >= b->len)
		b->off = b->len = 0;   /* the common case: nothing to move */
}

void
buf_compact(Buf *b)
{
	if (!b->off)
		return;
	if (b->len > b->off)
		memmove(b->p, b->p + b->off, b->len - b->off);
	b->len -= b->off;
	b->off = 0;
}

void
buf_trim(Buf *b, size_t keep)
{
	if (buf_used(b) == 0 && b->cap > keep) {
		b->off = b->len = 0;
		free(b->p);
		b->p = NULL;
		b->cap = 0;
	}
}

/* Splice n bytes in at an absolute index, moving what follows up.  Used
 * to put a header in front of a body that is already in the buffer, when
 * its length was not known until it had been written. */
void
buf_insert(Buf *b, size_t at, const void *p, size_t n)
{
	size_t tail = b->len - at;

	/* Nothing to splice in, and b->p may not exist yet - see buf_put. */
	if (!n)
		return;
	buf_grow(b, n, 0);
	if (tail)
		memmove(b->p + at + n, b->p + at, tail);
	memcpy(b->p + at, p, n);
	b->len += n;
}

void
buf_put(Buf *b, const void *p, size_t n)
{
	/* An empty value - a zero length queue message, a map field stored
	 * with no bytes - is the one input that reaches here asking for
	 * nothing.  buf_grow has nothing to do for it either, so the buffer
	 * of a connection whose first response this is has no allocation
	 * yet, and memcpy(NULL, p, 0) is undefined however little it does.
	 * Returning early is both the correct answer and the cheaper one. */
	if (!n)
		return;
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
