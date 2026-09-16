/* kache - a growable byte buffer */
#ifndef KACHE_BUF_H
#define KACHE_BUF_H

#include <string.h>

#include "util/util.h"

typedef struct Buf {
	u8    *p;
	size_t len;
	size_t cap;
} Buf;

void buf_free(Buf *b);
/* make room for n more bytes; returns 0 or -1 if that would exceed max */
int  buf_grow(Buf *b, size_t n, size_t max);
/* drop the first n bytes */
void buf_drain(Buf *b, size_t n);
/* release an oversized allocation once the buffer is empty */
void buf_trim(Buf *b, size_t keep);

void buf_put(Buf *b, const void *p, size_t n);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s);
void buf_putu(Buf *b, u64 v);
void buf_puti(Buf *b, i64 v);

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
