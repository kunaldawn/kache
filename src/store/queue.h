/* kache - the queue: a deque that costs one allocation per segment
 *
 * A queue could have been a linked list of records, one allocator call
 * and one index slot per message.  Instead entries live inside segments:
 * an ordinary arena block holding a run of framed entries, filled from
 * whichever end is being pushed.  One allocation feeds dozens of pushes,
 * the bytes of a queue being drained stay contiguous, and the index
 * never learns that the queue has more than one key in it.
 *
 * Each entry carries its frame length at both ends, so a pop from either
 * side is a constant time step - the same boundary tag idea the arena
 * allocator uses one level down.
 *
 * Like the nested map, a queue lives entirely inside one shard, so a
 * push, a pop and a move between two queues are atomic without any
 * second lock.  Every function here runs under the shard lock, and every
 * one that can allocate expects the record to be pinned. */
#ifndef KACHE_QUEUE_H
#define KACHE_QUEUE_H

#include "store/map.h"
#include "store/store.h"
#include "util/util.h"

typedef struct QIter {
	Ref seg;
	u32 pos;
	int right;     /* walking from the tail towards the head */
} QIter;

static inline QHdr *
q_hdr(Rec *r)
{
	return (QHdr *)cont_ctl(r);
}

static inline QSeg *
q_seg(const Map *m, const Shard *s, Ref ref)
{
	return (QSeg *)map_at(m, sub_off(s, ref));
}

static inline int
qent_expired(const QEnt *e, u64 now)
{
	return e->expire != 0 && e->expire <= now;
}

/* lay out an empty queue in a record's value bytes */
void  q_init(Rec *r);

/* Append vl bytes at one end.  The entry header is filled in, the value
 * is the caller's to write into the returned entry.  NULL when the arena
 * could not make room. */
QEnt *q_push(Map *m, Shard *s, Rec *r, int right, u32 vl, u64 expire,
             u32 flags);

/* the entry at one end, or NULL when the queue is empty */
QEnt *q_end(const Map *m, const Shard *s, Rec *r, int right);
/* remove it */
void  q_take(Map *m, Shard *s, Rec *r, int right);

/* Drop expired entries from both ends, at most budget of them.  An
 * expired entry in the middle of a queue is skipped by a reader and
 * reclaimed when the entries in front of it are gone; the layout that
 * makes a push cost nothing is the same one that cannot cut a hole. */
u32   q_expire(Map *m, Shard *s, Rec *r, u64 now, u32 budget);
/* take entries off one end until at most maxlen are left */
u64   q_trim(Map *m, Shard *s, Rec *r, u64 maxlen, int right);

void  q_iter(QIter *it, const Map *m, const Shard *s, Rec *r, int right);
QEnt *q_iter_next(const Map *m, const Shard *s, QIter *it);

/* incremental teardown; 1 once nothing is left */
int   q_reap(Map *m, Shard *s, Rec *r, u32 *budget);
/* recovery: validate the segment chain and mark it.  0 ok, -1 throw away */
int   q_check(Map *m, Shard *s, Rec *r, u64 now);

#endif /* KACHE_QUEUE_H */
