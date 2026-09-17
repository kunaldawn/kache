/* kache - a growable byte buffer with a read cursor
 *
 * Live bytes are p[off .. len).  Consuming advances the cursor instead
 * of moving the remainder down, which matters more than it sounds: a
 * pipelined batch used to memmove the rest of the buffer once per
 * request, so the cost of draining N buffered requests was quadratic in
 * N.  Now the bytes move at most once per read syscall.
 *
 * Compaction is deliberately explicit and never happens inside
 * buf_grow().  Response building records absolute indexes into the
 * buffer while a body is assembled (see http_wrap), and shifting the
 * contents underneath it would invalidate them.  Growing may realloc,
 * which preserves those indexes; compacting would not. */
#ifndef KACHE_BUF_H
#define KACHE_BUF_H

#include <string.h>

#include "util/util.h"

typedef struct Buf {
	u8    *p;
	size_t off;    /* read cursor */
	size_t len;    /* end of the written bytes */
	size_t cap;
} Buf;

void buf_free(Buf *b);
/* Make room for n more bytes.  0 when there is now room for all n; -1
 * when max (0 = no limit) stopped it short, in which case the buffer may
 * still have grown and buf_room() says by how much. */
int  buf_grow(Buf *b, size_t n, size_t max);
/* consume n bytes from the front */
void buf_drain(Buf *b, size_t n);
/* move the unread bytes back to the start; see the note above */
void buf_compact(Buf *b);
/* release an oversized allocation once the buffer is empty */
void buf_trim(Buf *b, size_t keep);

void buf_insert(Buf *b, size_t at, const void *p, size_t n);
void buf_put(Buf *b, const void *p, size_t n);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s);
void buf_putu(Buf *b, u64 v);
void buf_puti(Buf *b, i64 v);

/* the unread bytes */
static inline u8 *
buf_data(Buf *b)
{
	return b->p + b->off;
}

static inline size_t
buf_used(const Buf *b)
{
	return b->len - b->off;
}

/* where the next write lands, and how much fits there */
static inline u8 *
buf_tail(Buf *b)
{
	return b->p + b->len;
}

static inline size_t
buf_room(const Buf *b)
{
	return b->cap - b->len;
}

#endif /* KACHE_BUF_H */
