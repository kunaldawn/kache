/* kache - the per connection state machine: read, parse, answer, write. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "config.h"
#include "http/conn.h"
#include "http/http.h"
#include "util/clk.h"

#define READ_CHUNK 16384

/* ---- pool ------------------------------------------------------------ */

void
pool_init(Pool *p, u32 cap)
{
	u32 i;

	memset(p, 0, sizeof(*p));
	p->slots = ecalloc(cap, sizeof(Conn));
	p->cap = cap;
	for (i = cap; i-- > 0;) {
		p->slots[i].fd = -1;
		p->slots[i].fnext = p->freelist;
		p->freelist = &p->slots[i];
	}
}

void
pool_fini(Pool *p)
{
	u32 i;

	for (i = 0; i < p->cap; i++) {
		if (p->slots[i].open) {
			close(p->slots[i].fd);
			buf_free(&p->slots[i].in);
			buf_free(&p->slots[i].out);
		}
	}
	free(p->slots);
	memset(p, 0, sizeof(*p));
}

static void
list_unlink(Pool *p, Conn *c)
{
	if (c->prev)
		c->prev->next = c->next;
	else
		p->head = c->next;
	if (c->next)
		c->next->prev = c->prev;
	else
		p->tail = c->prev;
	c->prev = c->next = NULL;
}

static void
list_append(Pool *p, Conn *c)
{
	c->prev = p->tail;
	c->next = NULL;
	if (p->tail)
		p->tail->next = c;
	else
		p->head = c;
	p->tail = c;
}

void
conn_touch(Pool *p, Conn *c, u64 now)
{
	c->atime = now;
	if (p->tail == c)
		return;
	list_unlink(p, c);
	list_append(p, c);
}

Conn *
pool_oldest(Pool *p)
{
	return p->head;
}

static int
arm(Conn *c, u32 events)
{
	struct epoll_event ev;

	if (c->events == events)
		return 0;
	ev.events = events;
	ev.data.ptr = c;
	if (epoll_ctl(c->epfd, c->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
	              c->fd, &ev) < 0)
		return -1;
	c->events = events;
	return 0;
}

Conn *
conn_open(Pool *p, int fd, int epfd, u64 now)
{
	Conn *c = p->freelist;

	if (!c)
		return NULL;
	p->freelist = c->fnext;
	c->fnext = NULL;
	c->fd = fd;
	c->epfd = epfd;
	c->events = 0;
	c->in.off = c->in.len = 0;
	c->out.off = c->out.len = 0;
	c->open = 1;
	c->closing = 0;
	c->continued = 0;
	c->atime = now;
	p->used++;
	list_append(p, c);
	if (arm(c, EPOLLIN | EPOLLRDHUP) < 0) {
		conn_close(p, c, NULL);
		return NULL;
	}
	return c;
}

void
conn_close(Pool *p, Conn *c, Stats *st)
{
	if (!c->open)
		return;
	if (c->events)
		epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
	c->events = 0;
	c->open = 0;
	list_unlink(p, c);
	buf_free(&c->in);
	buf_free(&c->out);
	c->fnext = p->freelist;
	p->freelist = c;
	p->used--;
	if (st) {
		st_inc(&st->closed);
		st_dec(&st->current);
	}
}

/* ---- request processing ---------------------------------------------- */

static void
bail(Conn *c, int status)
{
	http_simple(&c->out, status, clk_date(), 0);
	c->closing = 1;
}

static void
process(Conn *c, Ctx *ctx)
{
	for (;;) {
		Req r;
		size_t total;
		int n, ka;

		if (c->closing || !buf_used(&c->in))
			return;
		n = http_head((const char *)buf_data(&c->in),
		              buf_used(&c->in), &r);
		if (n == 0) {
			if (buf_used(&c->in) >= ctx->max_req)
				bail(c, 431);
			return;
		}
		if (n < 0) {
			st_inc(&ctx->st->errors);
			bail(c, -n);
			return;
		}
		total = (size_t)n + r.clen;
		if (r.clen > ctx->max_req || total > ctx->max_req) {
			st_inc(&ctx->st->errors);
			bail(c, 413);
			return;
		}
		if (buf_used(&c->in) < total) {
			if (r.expect100 && !c->continued) {
				buf_puts(&c->out,
				    "HTTP/1.1 100 Continue\r\n\r\n");
				c->continued = 1;
			}
			return;
		}
		r.body.p = (const char *)buf_data(&c->in) + n;
		r.body.n = r.clen;
		c->continued = 0;

		route(ctx, &r, &c->out, &ka);
		buf_drain(&c->in, total);
		if (!ka) {
			c->closing = 1;
			return;
		}
		if (buf_used(&c->out) >= CFG_OUT_HIGH)
			return;
	}
}

static int
do_read(Conn *c, Ctx *ctx)
{
	ssize_t n;

	/* Reclaim what has already been parsed before asking for more
	 * room: one memmove per read syscall, not one per request. */
	if (buf_room(&c->in) < READ_CHUNK)
		buf_compact(&c->in);
	if (buf_room(&c->in) < READ_CHUNK &&
	    buf_grow(&c->in, READ_CHUNK, ctx->max_req) < 0) {
		if (buf_room(&c->in) == 0) {
			st_inc(&ctx->st->errors);
			bail(c, 413);
			return 0;
		}
	}
	for (;;) {
		n = read(c->fd, buf_tail(&c->in), buf_room(&c->in));
		if (n > 0) {
			c->in.len += (size_t)n;
			st_add(&ctx->st->bytes_in, (u64)n);
			return 0;
		}
		if (n == 0)
			return -1;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
}

static int
do_write(Conn *c, Ctx *ctx)
{
	while (buf_used(&c->out)) {
		ssize_t n = write(c->fd, buf_data(&c->out),
		                  buf_used(&c->out));

		if (n > 0) {
			buf_drain(&c->out, (size_t)n);
			st_add(&ctx->st->bytes_out, (u64)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return -1;
	}
	buf_trim(&c->out, CFG_BUF_KEEP);
	return 0;
}

int
conn_event(Conn *c, Ctx *ctx, u32 events)
{
	if (events & (EPOLLERR | EPOLLHUP))
		return -1;

	if (events & EPOLLOUT) {
		if (do_write(c, ctx) < 0)
			return -1;
	}
	if (events & (EPOLLIN | EPOLLRDHUP)) {
		if (do_read(c, ctx) < 0) {
			/* the peer is done sending; answer what we have
			 * and then drop the connection */
			process(c, ctx);
			do_write(c, ctx);
			return -1;
		}
	}
	process(c, ctx);
	if (do_write(c, ctx) < 0)
		return -1;

	if (buf_used(&c->out))
		return arm(c, EPOLLOUT | EPOLLRDHUP) < 0 ? -1 : 0;
	if (c->closing)
		return -1;
	/* more pipelined bytes may still be sitting in the read buffer */
	if (buf_used(&c->in)) {
		process(c, ctx);
		if (do_write(c, ctx) < 0)
			return -1;
		if (buf_used(&c->out))
			return arm(c, EPOLLOUT | EPOLLRDHUP) < 0 ? -1 : 0;
		if (c->closing)
			return -1;
	}
	buf_trim(&c->in, CFG_BUF_KEEP);
	return arm(c, EPOLLIN | EPOLLRDHUP) < 0 ? -1 : 0;
}
