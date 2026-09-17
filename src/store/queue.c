/* kache - the queue.  See queue.h for why it looks like this. */
#include <string.h>

#include "config.h"
#include "store/alloc.h"
#include "store/queue.h"
#include "store/shard.h"

void
q_init(Rec *r)
{
	memset(cont_ctl(r), 0, sizeof(QHdr));
	r->vlen = (u32)sizeof(QHdr);
}

/* ---- segments -------------------------------------------------------- */

/* Unlink and free a segment.  Only ever called on an empty one. */
static void
seg_drop(Map *m, Shard *s, QHdr *q, Ref ref)
{
	QSeg *g = q_seg(m, s, ref);

	if (g->prev)
		q_seg(m, s, g->prev)->next = g->next;
	else
		q->head = g->next;
	if (g->next)
		q_seg(m, s, g->next)->prev = g->prev;
	else
		q->tail = g->prev;
	alc_free(m, s, sub_off(s, ref), NULL, NULL);
}

/* Link a fresh segment of at least need usable bytes onto one end.  An
 * empty queue grows its segments geometrically: a queue of five messages
 * should not pay for a queue of five million, and one of five million
 * should not pay an allocation every few pushes. */
static QSeg *
seg_new(Map *m, Shard *s, Rec *r, int right, u32 need)
{
	QHdr *q = q_hdr(r);
	u32 want = q->segsz ? q->segsz : CFG_QSEG_MIN;
	QSeg *g;
	u64 off;

	if (want > CFG_QSEG_MAX)
		want = CFG_QSEG_MAX;
	if (want < need)
		want = need;              /* one entry, its own segment */
	if (!(off = shd_alloc_sub(m, s, (u32)sizeof(QSeg) + want)))
		return NULL;
	q = q_hdr(r);
	g = (QSeg *)map_at(m, off);
	g->owner = sub_ref(s, map_off(m, r));
	/* take whatever the allocator rounded up to, not just what we asked */
	g->cap = (u32)ALIGNDN(alc_cap(m, off) - (u32)sizeof(QSeg), 8u);
	g->count = 0;
	g->pad = 0;
	g->head = g->tail = right ? 0 : g->cap;
	g->next = g->prev = 0;
	if (right) {
		g->prev = q->tail;
		if (q->tail)
			q_seg(m, s, q->tail)->next = sub_ref(s, off);
		else
			q->head = sub_ref(s, off);
		q->tail = sub_ref(s, off);
	} else {
		g->next = q->head;
		if (q->head)
			q_seg(m, s, q->head)->prev = sub_ref(s, off);
		else
			q->tail = sub_ref(s, off);
		q->head = sub_ref(s, off);
	}
	q->segsz = want < CFG_QSEG_MAX ? want << 1 : CFG_QSEG_MAX;
	if (q->segsz > CFG_QSEG_MAX)
		q->segsz = CFG_QSEG_MAX;
	return g;
}

/* ---- push and pop ---------------------------------------------------- */

QEnt *
q_push(Map *m, Shard *s, Rec *r, int right, u32 vl, u64 expire, u32 flags)
{
	QHdr *q = q_hdr(r);
	u32 need = QENT_NEED(vl);
	Ref ref = right ? q->tail : q->head;
	QSeg *g = ref ? q_seg(m, s, ref) : NULL;
	QEnt *e;

	if (g && !g->count) {
		/* an empty segment can be aimed either way for free */
		g->head = g->tail = right ? 0 : g->cap;
	}
	if (!g || (right ? g->cap - g->tail : g->head) < need) {
		if (!(g = seg_new(m, s, r, right, need)))
			return NULL;
		q = q_hdr(r);
	}
	if (right) {
		e = (QEnt *)(g->data + g->tail);
		g->tail += need;
	} else {
		g->head -= need;
		e = (QEnt *)(g->data + g->head);
	}
	e->len = need;
	e->vlen = vl;
	e->expire = expire;
	e->id = ++q->seq;
	e->flags = flags;
	e->pad = 0;
	*qent_back(e) = need;
	g->count++;
	q->count++;
	q->bytes += vl;
	return e;
}

QEnt *
q_end(const Map *m, const Shard *s, Rec *r, int right)
{
	QHdr *q = q_hdr(r);
	QSeg *g;

	if (!q->count)
		return NULL;
	g = q_seg(m, s, right ? q->tail : q->head);
	if (right)
		return (QEnt *)(g->data + g->tail -
		                *(u32 *)(g->data + g->tail - 4));
	return (QEnt *)(g->data + g->head);
}

void
q_take(Map *m, Shard *s, Rec *r, int right)
{
	QHdr *q = q_hdr(r);
	Ref ref = right ? q->tail : q->head;
	QSeg *g = q_seg(m, s, ref);
	QEnt *e;

	if (right) {
		e = (QEnt *)(g->data + g->tail -
		             *(u32 *)(g->data + g->tail - 4));
		g->tail -= e->len;
	} else {
		e = (QEnt *)(g->data + g->head);
		g->head += e->len;
	}
	g->count--;
	q->count--;
	q->bytes -= e->vlen;
	/* Keep one empty segment around so a queue that hovers at zero
	 * does not allocate and free on every message; anything oversized
	 * goes back, since it was sized for one large entry. */
	if (!g->count && (g->next || g->prev || g->cap > CFG_QSEG_KEEP))
		seg_drop(m, s, q, ref);
	else if (!g->count)
		g->head = g->tail = 0;
}

u32
q_expire(Map *m, Shard *s, Rec *r, u64 now, u32 budget)
{
	u32 n = 0;
	int right;

	for (right = 0; right <= 1; right++) {
		while (n < budget) {
			QEnt *e = q_end(m, s, r, right);

			if (!e || !qent_expired(e, now))
				break;
			q_take(m, s, r, right);
			s->expirations++;
			n++;
		}
	}
	return n;
}

u64
q_trim(Map *m, Shard *s, Rec *r, u64 maxlen, int right)
{
	QHdr *q = q_hdr(r);
	u64 n = 0;

	while (q->count > maxlen) {
		q_take(m, s, r, right);
		n++;
	}
	return n;
}

/* ---- walking --------------------------------------------------------- */

void
q_iter(QIter *it, const Map *m, const Shard *s, Rec *r, int right)
{
	QHdr *q = q_hdr(r);

	it->right = right;
	it->seg = right ? q->tail : q->head;
	if (it->seg) {
		QSeg *g = q_seg(m, s, it->seg);

		it->pos = right ? g->tail : g->head;
	} else {
		it->pos = 0;
	}
}

QEnt *
q_iter_next(const Map *m, const Shard *s, QIter *it)
{
	QEnt *e;

	while (it->seg) {
		QSeg *g = q_seg(m, s, it->seg);

		if (it->right) {
			if (it->pos > g->head) {
				u32 back = *(u32 *)(g->data + it->pos - 4);

				it->pos -= back;
				return (QEnt *)(g->data + it->pos);
			}
			if ((it->seg = g->prev) != 0)
				it->pos = q_seg(m, s, it->seg)->tail;
		} else {
			if (it->pos < g->tail) {
				e = (QEnt *)(g->data + it->pos);
				it->pos += e->len;
				return e;
			}
			if ((it->seg = g->next) != 0)
				it->pos = q_seg(m, s, it->seg)->head;
		}
	}
	return NULL;
}

/* ---- teardown and recovery ------------------------------------------- */

int
q_reap(Map *m, Shard *s, Rec *r, u32 *budget)
{
	QHdr *q = q_hdr(r);

	while (q->head) {
		Ref nx;

		if (!*budget)
			return 0;
		(*budget)--;
		nx = q_seg(m, s, q->head)->next;
		alc_free(m, s, sub_off(s, q->head), NULL, NULL);
		q->head = nx;
	}
	q->tail = 0;
	q->count = 0;
	q->bytes = 0;
	return 1;
}

static void
unclaim(Map *m, const Shard *s, Ref from, u64 n)
{
	while (from && n--) {
		QSeg *g = q_seg(m, s, from);

		alc_unmark(m, sub_off(s, from), BLK_MARK);
		from = g->next;
	}
}

int
q_check(Map *m, Shard *s, Rec *r, u64 now)
{
	QHdr *q;
	Ref self = sub_ref(s, map_off(m, r)), cur, prev = 0;
	u64 nseg = 0, done = 0, limit, count = 0, bytes = 0;

	(void)now;
	if (r->vlen != sizeof(QHdr))
		return -1;
	q = q_hdr(r);
	q->gnext = 0;
	q->gpos = 0;
	if (!q->head) {
		if (q->tail)
			return -1;
		q->count = 0;
		q->bytes = 0;
		return 0;
	}
	limit = s->arena_size / ST_MINBLK + 1;
	for (cur = q->head; cur; ) {
		QSeg *g;
		u64 off;
		u32 p, c = 0;

		if (!(off = shd_subok(m, s, cur, (u32)sizeof(QSeg))))
			goto bad;
		if (++nseg > limit)
			goto bad;
		g = (QSeg *)map_at(m, off);
		if (g->owner != self || g->prev != prev)
			goto bad;
		if ((u64)sizeof(QSeg) + g->cap + ST_BLKHDR >
		    BLK_SIZE(alc_of(m, off)))
			goto bad;
		if ((g->cap & 7) || (g->head & 7) || (g->tail & 7) ||
		    g->head > g->tail || g->tail > g->cap)
			goto bad;
		for (p = g->head; p < g->tail; ) {
			QEnt *e = (QEnt *)(g->data + p);

			if (e->len < QENT_HDR + 4 || (e->len & 7) ||
			    (u64)p + e->len > g->tail)
				goto bad;
			if (e->vlen > m->maxval || QENT_NEED(e->vlen) != e->len ||
			    *qent_back(e) != e->len)
				goto bad;
			bytes += e->vlen;
			c++;
			p += e->len;
		}
		if (p != g->tail || c != g->count)
			goto bad;
		count += c;
		alc_mark(m, off, BLK_MARK);
		done++;
		prev = cur;
		cur = g->next;
	}
	if (prev != q->tail)
		goto bad;
	q->count = count;
	q->bytes = bytes;
	return 0;
bad:
	unclaim(m, s, q->head, done);
	return -1;
}
