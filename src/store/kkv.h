/* kache - the nested map: a key whose value is itself a key/value store
 *
 * The outer key picks the shard, so every field of one map lives under
 * one lock: a read, a write and a whole map enumeration are all atomic
 * against each other with no second level of synchronisation, and a
 * multi field update is atomic too, which is the one thing a flattened
 * "outer\0inner" key space cannot give you.
 *
 * The map itself is open addressed, like the shard index, but its slots
 * are half the size: a small table needs no more than the low 32 bits of
 * the hash to reject a collision, and the low bits of that tag also say
 * where the entry belongs, which is what backward shift deletion needs.
 * Eight slots to a cache line means most lookups touch one line.
 *
 * Growing rehashes the slot array alone.  The tag carries everything the
 * new position depends on, so not one field record is read.
 *
 * Every function here runs under the shard lock, and every one that can
 * allocate expects the owning record to be pinned - see shd_pin(). */
#ifndef KACHE_KKV_H
#define KACHE_KKV_H

#include "store/map.h"
#include "store/store.h"
#include "util/util.h"

static inline KkvHdr *
kkv_hdr(Rec *r)
{
	return (KkvHdr *)cont_ctl(r);
}

static inline KkvFld *
kkv_at(const Map *m, const Shard *s, Ref ref)
{
	return (KkvFld *)map_at(m, sub_off(s, ref));
}

static inline int
fld_expired(const KkvFld *f, u64 now)
{
	return f->expire != 0 && f->expire <= now;
}

/* lay out an empty map in a record's value bytes */
void    kkv_init(Rec *r);

/* find a field; slot is only meaningful on a hit */
KkvFld *kkv_find(const Map *m, const Shard *s, const KkvHdr *h, u64 fh,
                 const void *f, u32 fl, u32 *slot);

/* Install a field of vl value bytes, replacing any existing one.  The
 * key is copied in; the value is the caller's to write.  NULL when the
 * arena could not make room, in which case nothing changed. */
KkvFld *kkv_put(Map *m, Shard *s, Rec *r, u64 fh, const void *f, u32 fl,
                u32 vl, int *created);

/* remove the field occupying a slot */
void    kkv_erase(Map *m, Shard *s, Rec *r, u32 slot);

/* Hand back a slot table the map has outgrown downwards.  Never called
 * from kkv_erase: an enumeration erases expired fields as it goes and
 * must not have the array move underneath it. */
void    kkv_compact(Map *m, Shard *s, Rec *r);

/* Walk live fields.  Start with *cur 0 and call until it returns NULL;
 * expired fields met on the way are dropped rather than reported. */
KkvFld *kkv_walk(Map *m, Shard *s, Rec *r, u64 now, u32 *cur, u32 *slot);

/* Take the map apart a piece at a time, spending at most *budget units.
 * Returns 1 once nothing is left of it. */
int     kkv_reap(Map *m, Shard *s, Rec *r, u32 *budget);

/* recovery: validate the whole subtree and mark it reachable.  0 if the
 * map is sound, -1 if the record should be thrown away */
int     kkv_check(Map *m, Shard *s, Rec *r, u64 now);

#endif /* KACHE_KKV_H */
