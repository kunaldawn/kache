/* kache - the operations.  Each one takes the single lock covering its
 * key, which is what makes it atomic against every other operation. */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "store/alloc.h"
#include "store/db.h"
#include "store/kkv.h"
#include "store/queue.h"
#include "store/shard.h"
#include "util/clk.h"
#include "util/hash.h"

int
db_open(Db *db, const MapCfg *cfg)
{
	return map_open(&db->map, cfg);
}

void
db_close(Db *db)
{
	map_close(&db->map);
}

int
db_sync(Db *db, int wait)
{
	return map_sync(&db->map, wait);
}

const char *
db_strerror(int rc)
{
	switch (rc) {
	case DB_OK:     return "ok";
	case DB_ENOENT: return "not found";
	case DB_EEXIST: return "already exists";
	case DB_ECAS:   return "version mismatch";
	case DB_ENOSPC: return "out of space";
	case DB_E2BIG:  return "too large";
	case DB_ENUM:   return "not a number";
	case DB_ESMALL: return "buffer too small";
	case DB_ETYPE:  return "wrong type for this key";
	}
	return "unknown";
}

static inline i64
ttl_left(const Rec *r, u64 now)
{
	if (!r->expire)
		return DB_FOREVER;
	return r->expire > now ? (i64)(r->expire - now) : 0;
}

static inline u64
ttl_abs(i64 ttl, u64 now)
{
	return ttl < 0 ? 0 : now + (u64)ttl;
}

static void
meta_of(DbMeta *meta, const Rec *r, u64 now, int created)
{
	if (!meta)
		return;
	meta->version = r->version;
	meta->flags = r->flags;
	meta->vlen = r->vlen;
	meta->ttl = ttl_left(r, now);
	meta->created = created;
}

/* Every operation lends the sweeper a hand on the way in.  Dismantling
 * a container that has been deleted is spread across the traffic that
 * follows it, so no single request pays for the whole of it. */
static inline void
sweep_some(Map *m, Shard *s)
{
	if (UNLIKELY(s->dead != 0))
		shd_sweep(m, s, CFG_SWEEP_BUDGET);
}

static Rec *
live(Map *m, Shard *s, u64 h, const void *k, u32 kl, u64 now, u32 *slot)
{
	Rec *r = shd_lookup(m, s, h, k, kl, slot);

	if (r && rec_expired(r, now)) {
		s->expirations++;
		shd_drop(m, s, *slot);
		return NULL;
	}
	return r;
}

int
db_get(Db *db, const void *k, u32 kl, void *out, u32 cap, DbMeta *meta)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
	} else if (r->type != RT_KV) {
		rc = DB_ETYPE;
	} else if (r->vlen > cap) {
		meta_of(meta, r, now, 0);
		rc = DB_ESMALL;
	} else {
		u32 sec = (u32)(now / 1000);

		/* Stamping every hit dirties the record's cache line and
		 * the mmap page under it, so a pure read hands the next
		 * msync a page to write back.  Sampled LRU only ranks
		 * atimes against each other, so letting one lag a few
		 * seconds costs it nothing.  The difference is unsigned on
		 * purpose: a record stamped ahead of the clock - an NTP
		 * step back, or a file written on a machine that was
		 * further ahead - wraps to a huge difference and so is
		 * restamped on the next read, instead of going untouched
		 * until the clock catches up.  The branch is preprocessor
		 * rather than plain code because at slack 0 the comparison
		 * is always true, and -Wextra says so. */
#if CFG_ATIME_SLACK
		if ((u32)(sec - r->atime) >= (u32)CFG_ATIME_SLACK)
			r->atime = sec;
#else
		r->atime = sec;
#endif
		memcpy(out, rec_val(r), r->vlen);
		meta_of(meta, r, now, 0);
	}
	lock_release(&s->lock);
	return rc;
}

/* Install a fresh record for a key, replacing whatever is there.  The
 * allocation happens before the old record goes away, because making
 * room may evict arbitrary records - including the one being replaced -
 * so the lookup has to be redone afterwards. */
static Rec *
install(Map *m, Shard *s, u64 h, const void *k, u32 kl, u32 vl, int creating,
        u8 type)
{
	u32 need = type == RT_KV ? REC_NEED(kl, vl) : CONT_NEED(kl, vl);
	u64 off;
	u32 slot;
	Rec *old, *r;

	if (creating)
		shd_reserve(m, s);
	if (!(off = shd_alloc(m, s, need)))
		return NULL;
	if ((old = shd_lookup(m, s, h, k, kl, &slot)) != NULL)
		shd_drop(m, s, slot);

	r = (Rec *)map_at(m, off);
	r->hash = h;
	r->klen = (u16)kl;
	r->type = type;
	r->pad = 0;
	r->vlen = vl;
	memcpy(r->data, k, kl);
	shd_link(m, s, h, off);
	return r;
}

int
db_set(Db *db, const void *k, u32 kl, const void *v, u32 vl,
       i64 ttl, u32 flags, int mode, u64 cas, DbMeta *meta)
{
	Map *m = &db->map;
	u64 h, now = now_ms(), expire;
	Shard *s;
	Rec *r;
	u32 slot, need;
	int created = 0, rc = DB_OK;

	if (!kl || kl > m->maxkey || vl > m->maxval)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);
	need = REC_NEED(kl, vl);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	r = live(m, s, h, k, kl, now, &slot);
	if (r) {
		if (r->type != RT_KV) {
			rc = DB_ETYPE;
			goto out;
		}
		if (mode & SET_ADD) {
			rc = DB_EEXIST;
			goto out;
		}
		if ((mode & SET_CAS) && r->version != cas) {
			rc = DB_ECAS;
			goto out;
		}
		expire = (mode & SET_KEEPTTL) ? r->expire : ttl_abs(ttl, now);
		if (alc_fits(alc_cap(m, map_off(m, r)), need)) {
			r->vlen = vl;
			memcpy(rec_val(r), v, vl);
			goto commit;
		}
	} else {
		if (mode & (SET_REPLACE | SET_CAS)) {
			rc = DB_ENOENT;
			goto out;
		}
		created = 1;
		expire = ttl_abs(ttl, now);
	}

	if (!(r = install(m, s, h, k, kl, vl, created, RT_KV))) {
		rc = DB_ENOSPC;
		goto out;
	}
	memcpy(rec_val(r), v, vl);
commit:
	r->expire = expire;
	r->flags = flags;
	r->atime = (u32)(now / 1000);
	r->version = ++s->verseq;
	meta_of(meta, r, now, created);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_del(Db *db, const void *k, u32 kl, int use_cas, u64 cas)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if (!(r = live(m, s, h, k, kl, now, &slot)))
		rc = DB_ENOENT;
	else if (use_cas && r->version != cas)
		rc = DB_ECAS;
	else
		shd_drop(m, s, slot);
	lock_release(&s->lock);
	return rc;
}

int
db_touch(Db *db, const void *k, u32 kl, i64 ttl, DbMeta *meta)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
	} else if (r->type != RT_KV) {
		rc = DB_ETYPE;
	} else {
		r->expire = ttl_abs(ttl, now);
		r->atime = (u32)(now / 1000);
		meta_of(meta, r, now, 0);
	}
	lock_release(&s->lock);
	return rc;
}

int
db_incr(Db *db, const void *k, u32 kl, i64 delta, i64 init,
        i64 ttl, int set_ttl, i64 *result, DbMeta *meta)
{
	Map *m = &db->map;
	u64 h, now = now_ms(), expire;
	Shard *s;
	Rec *r;
	char num[24];
	i64 cur, next;
	u32 slot, nl, flags = 0;
	int created = 0, rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	r = live(m, s, h, k, kl, now, &slot);
	if (r) {
		if (r->type != RT_KV) {
			rc = DB_ETYPE;
			goto out;
		}
		if (parse_i64((const char *)rec_val(r), r->vlen, &cur) < 0) {
			rc = DB_ENUM;
			goto out;
		}
		flags = r->flags;
		expire = set_ttl ? ttl_abs(ttl, now) : r->expire;
	} else {
		cur = init;
		created = 1;
		expire = ttl_abs(ttl, now);
	}
	if (__builtin_add_overflow(cur, delta, &next)) {
		rc = DB_ENUM;
		goto out;
	}
	nl = (u32)fmt_i64(num, next);

	if (r && alc_fits(alc_cap(m, map_off(m, r)), REC_NEED(kl, nl))) {
		r->vlen = nl;
		memcpy(rec_val(r), num, nl);
	} else {
		if (!(r = install(m, s, h, k, kl, nl, created, RT_KV))) {
			rc = DB_ENOSPC;
			goto out;
		}
		memcpy(rec_val(r), num, nl);
	}
	r->expire = expire;
	r->flags = flags;
	r->atime = (u32)(now / 1000);
	r->version = ++s->verseq;
	if (result)
		*result = next;
	meta_of(meta, r, now, created);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_cat(Db *db, const void *k, u32 kl, const void *v, u32 vl,
       int prepend, DbMeta *meta)
{
	Map *m = &db->map;
	u64 h, now = now_ms(), expire;
	Shard *s;
	Rec *r;
	u8 stack[4096], *tmp = NULL;
	u32 slot, nl, flags;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_KV) {
		rc = DB_ETYPE;
		goto out;
	}
	nl = r->vlen + vl;
	if (nl > m->maxval) {
		rc = DB_E2BIG;
		goto out;
	}
	expire = r->expire;
	flags = r->flags;

	if (!prepend && alc_fits(alc_cap(m, map_off(m, r)), REC_NEED(kl, nl))) {
		memcpy(rec_val(r) + r->vlen, v, vl);
		r->vlen = nl;
	} else {
		/* The old value has to survive an allocation that may
		 * evict it, so it is staged outside the mapping first. */
		u32 ol = r->vlen;

		tmp = ol <= sizeof(stack) ? stack : emalloc(ol);
		memcpy(tmp, rec_val(r), ol);
		if (!(r = install(m, s, h, k, kl, nl, 0, RT_KV))) {
			rc = DB_ENOSPC;
			goto out;
		}
		if (prepend) {
			memcpy(rec_val(r), v, vl);
			memcpy(rec_val(r) + vl, tmp, ol);
		} else {
			memcpy(rec_val(r), tmp, ol);
			memcpy(rec_val(r) + ol, v, vl);
		}
	}
	r->expire = expire;
	r->flags = flags;
	r->atime = (u32)(now / 1000);
	r->version = ++s->verseq;
	meta_of(meta, r, now, 0);
out:
	lock_release(&s->lock);
	if (tmp && tmp != stack)
		free(tmp);
	return rc;
}

void
db_flush(Db *db)
{
	Map *m = &db->map;
	u32 i;

	for (i = 0; i < m->nshards; i++) {
		Shard *s = &m->shards[i];

		lock_acquire(&s->lock);
		shd_clear(m, s);
		lock_release(&s->lock);
	}
}

/* Housekeeping, so it must never stand in front of a request: the lock
 * is taken with a try and a shard that is busy is simply left alone -
 * its own traffic is already sweeping it on the way through. */
u64
db_reclaim(Db *db, u32 from, u32 stride, u32 budget)
{
	Map *m = &db->map;
	u64 done = 0;
	u32 i;

	for (i = from; i < m->nshards; i += stride) {
		Shard *s = &m->shards[i];

		if (!lock_try(&s->lock))
			continue;
		if (s->dead)
			done += shd_sweep(m, s, budget);
		lock_release(&s->lock);
	}
	return done;
}

/* Each shard is sampled under its own lock, so the totals are a blend of
 * consistent per shard snapshots rather than a torn read of any one of
 * them.  Scraping is rare enough that the lock traffic does not matter. */
void
db_stats(Db *db, DbStats *st)
{
	Map *m = &db->map;
	u32 i;

	memset(st, 0, sizeof(*st));
	for (i = 0; i < m->nshards; i++) {
		Shard *s = &m->shards[i];

		lock_acquire(&s->lock);
		st->keys += s->count;
		st->bytes += s->used;
		st->capacity += s->arena_size;
		st->buckets += s->nbuckets;
		st->inserts += s->inserts;
		st->evictions += s->evictions;
		st->expirations += s->expirations;
		st->dead += s->dead_n;
		lock_release(&s->lock);
	}
}

/* ---- containers -------------------------------------------------------
 *
 * A container lives entirely inside the shard its outer key hashes to,
 * so one lock covers the whole of it: every call below is atomic against
 * every other operation on that key, however many fields or entries it
 * touches.  That is the difference between a real nested map and a
 * flattened "outer\0inner" key space, where the pieces of one map land
 * in different shards and nothing can be done to all of them at once.
 *
 * While one of these calls walks a container it pins the record, because
 * an allocation inside it may evict - and the one thing eviction must
 * not take is the container being walked.  Sub blocks are never
 * candidates in the first place, so pinning the record covers the whole
 * subtree hanging off it. */

static void
cont_meta(DbCont *ci, Rec *r, u64 now, int created)
{
	if (!ci)
		return;
	ci->version = r->version;
	ci->flags = r->flags;
	ci->ttl = ttl_left(r, now);
	ci->created = created;
	if (r->type == RT_KKV) {
		ci->count = kkv_hdr(r)->count;
		ci->bytes = kkv_hdr(r)->bytes;
		ci->seq = kkv_hdr(r)->fseq;
	} else {
		ci->count = q_hdr(r)->count;
		ci->bytes = q_hdr(r)->bytes;
		ci->seq = q_hdr(r)->seq;
	}
}

static void
fld_meta(DbMeta *meta, const KkvFld *f, u64 now, int created)
{
	if (!meta)
		return;
	meta->version = f->version;
	meta->flags = f->flags;
	meta->vlen = f->vlen;
	meta->ttl = !f->expire ? DB_FOREVER :
	    (f->expire > now ? (i64)(f->expire - now) : 0);
	meta->created = created;
}

static void
ent_meta(DbQMeta *em, const QEnt *e, u64 now)
{
	em->id = e->id;
	em->vlen = e->vlen;
	em->flags = e->flags;
	em->ttl = !e->expire ? DB_FOREVER :
	    (e->expire > now ? (i64)(e->expire - now) : 0);
}

static inline void
touch_atime(Rec *r, u64 now)
{
	u32 sec = (u32)(now / 1000);

#if CFG_ATIME_SLACK
	if ((u32)(sec - r->atime) >= (u32)CFG_ATIME_SLACK)
		r->atime = sec;
#else
	r->atime = sec;
#endif
}

/* Find what a key holds, creating an empty container when asked to. */
static int
cont_open(Map *m, Shard *s, u64 h, const void *k, u32 kl, u64 now, u8 type,
          int create, const DbKkvOpt *o, Rec **out, int *created)
{
	u32 slot;
	Rec *r = live(m, s, h, k, kl, now, &slot);

	*created = 0;
	if (r) {
		if (r->type != type)
			return DB_ETYPE;
		*out = r;
		return DB_OK;
	}
	if (!create)
		return DB_ENOENT;
	r = install(m, s, h, k, kl,
	            type == RT_KKV ? (u32)sizeof(KkvHdr) : (u32)sizeof(QHdr),
	            1, type);
	if (!r)
		return DB_ENOSPC;
	if (type == RT_KKV)
		kkv_init(r);
	else
		q_init(r);
	r->expire = ttl_abs(o->kttl, now);
	r->flags = o->flags;
	r->atime = (u32)(now / 1000);
	r->version = ++s->verseq;
	*out = r;
	*created = 1;
	return DB_OK;
}

/* A container with nothing left in it stops existing, the way an empty
 * hash or list does in redis: there is no state left to describe. */
static void
cont_gc(Map *m, Shard *s, Rec *r, u64 h, const void *k, u32 kl)
{
	u32 slot;

	if ((r->type == RT_KKV ? kkv_hdr(r)->count : q_hdr(r)->count) != 0)
		return;
	if (shd_lookup(m, s, h, k, kl, &slot) == r)
		shd_drop(m, s, slot);
}

/* ---- nested maps ----------------------------------------------------- */

static int
kkv_start(Db *db, const void *k, u32 kl, const void *f, u32 fl)
{
	const Map *m = &db->map;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	if (f && (!fl || fl > m->maxkey))
		return DB_E2BIG;
	return DB_OK;
}

int
db_kkv_get(Db *db, const void *k, u32 kl, const void *f, u32 fl,
           void *out, u32 cap, DbMeta *meta)
{
	Map *m = &db->map;
	u64 h, fh, now = now_ms();
	Shard *s;
	Rec *r;
	KkvFld *fd;
	u32 slot, fslot;
	int rc;

	if ((rc = kkv_start(db, k, kl, f, fl)) != DB_OK)
		return rc;
	h = hash_bytes(k, kl, m->seed);
	fh = hash_bytes(f, fl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
	} else if (r->type != RT_KKV) {
		rc = DB_ETYPE;
	} else if (!(fd = kkv_find(m, s, kkv_hdr(r), fh, f, fl, &fslot))) {
		rc = DB_ENOENT;
	} else if (fld_expired(fd, now)) {
		s->expirations++;
		kkv_erase(m, s, r, fslot);
		cont_gc(m, s, r, h, k, kl);
		rc = DB_ENOENT;
	} else if (fd->vlen > cap) {
		fld_meta(meta, fd, now, 0);
		rc = DB_ESMALL;
	} else {
		memcpy(out, fld_val(fd), fd->vlen);
		fld_meta(meta, fd, now, 0);
		touch_atime(r, now);
		rc = DB_OK;
	}
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_set(Db *db, const void *k, u32 kl, const void *f, u32 fl,
           const void *v, u32 vl, const DbKkvOpt *o, DbMeta *meta,
           DbCont *ci)
{
	Map *m = &db->map;
	u64 h, fh, now = now_ms(), fexp;
	Shard *s;
	Rec *r = NULL;
	KkvFld *fd;
	u32 fslot;
	int created = 0, fcreated = 0, rc;

	if ((rc = kkv_start(db, k, kl, f, fl)) != DB_OK)
		return rc;
	if (vl > m->maxval)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	fh = hash_bytes(f, fl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	rc = cont_open(m, s, h, k, kl, now, RT_KKV,
	               !(o->mode & (SET_REPLACE | SET_CAS)), o, &r, &created);
	if (rc != DB_OK)
		goto out;
	shd_pin(s, 0, map_off(m, r));

	fd = kkv_find(m, s, kkv_hdr(r), fh, f, fl, &fslot);
	if (fd && fld_expired(fd, now)) {
		s->expirations++;
		kkv_erase(m, s, r, fslot);
		fd = NULL;
	}
	if (fd) {
		if (o->mode & SET_ADD) {
			rc = DB_EEXIST;
			goto done;
		}
		if ((o->mode & SET_CAS) && fd->version != o->cas) {
			rc = DB_ECAS;
			goto done;
		}
	} else if (o->mode & (SET_REPLACE | SET_CAS)) {
		rc = DB_ENOENT;
		goto done;
	}
	fexp = (fd && (o->mode & SET_KEEPTTL)) ? fd->expire
	                                       : ttl_abs(o->ttl, now);
	if (!(fd = kkv_put(m, s, r, fh, f, fl, vl, &fcreated))) {
		rc = DB_ENOSPC;
		goto done;
	}
	memcpy(fld_val(fd), v, vl);
	fd->expire = fexp;
	fd->flags = o->flags;
	fd->version = ++kkv_hdr(r)->fseq;
	if (o->set_kttl)
		r->expire = ttl_abs(o->kttl, now);
	r->version = ++s->verseq;
	r->atime = (u32)(now / 1000);
	fld_meta(meta, fd, now, fcreated);
done:
	shd_unpin(s, 0);
	cont_meta(ci, r, now, created);
	if (rc != DB_OK)
		cont_gc(m, s, r, h, k, kl);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_del(Db *db, const void *k, u32 kl, const void *f, u32 fl,
           int use_cas, u64 cas, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, fh, now = now_ms();
	Shard *s;
	Rec *r;
	KkvFld *fd;
	u32 slot, fslot;
	int rc = DB_OK;

	if ((rc = kkv_start(db, k, kl, f, fl)) != DB_OK)
		return rc;
	h = hash_bytes(k, kl, m->seed);
	fh = hash_bytes(f, fl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_KKV) {
		rc = DB_ETYPE;
		goto out;
	}
	fd = kkv_find(m, s, kkv_hdr(r), fh, f, fl, &fslot);
	if (fd && fld_expired(fd, now)) {
		s->expirations++;
		kkv_erase(m, s, r, fslot);
		fd = NULL;
	}
	if (!fd) {
		rc = DB_ENOENT;
	} else if (use_cas && fd->version != cas) {
		rc = DB_ECAS;
	} else {
		kkv_erase(m, s, r, fslot);
		shd_pin(s, 0, map_off(m, r));
		kkv_compact(m, s, r);
		shd_unpin(s, 0);
		r->version = ++s->verseq;
	}
	cont_meta(ci, r, now, 0);
	cont_gc(m, s, r, h, k, kl);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_incr(Db *db, const void *k, u32 kl, const void *f, u32 fl,
            i64 delta, i64 init, const DbKkvOpt *o, i64 *result,
            DbMeta *meta, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, fh, now = now_ms(), fexp;
	Shard *s;
	Rec *r = NULL;
	KkvFld *fd;
	char num[24];
	i64 cur, next;
	u32 fslot, nl, flags;
	int created = 0, fcreated = 0, rc;

	if ((rc = kkv_start(db, k, kl, f, fl)) != DB_OK)
		return rc;
	h = hash_bytes(k, kl, m->seed);
	fh = hash_bytes(f, fl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if ((rc = cont_open(m, s, h, k, kl, now, RT_KKV, 1, o, &r,
	                    &created)) != DB_OK)
		goto out;
	shd_pin(s, 0, map_off(m, r));

	fd = kkv_find(m, s, kkv_hdr(r), fh, f, fl, &fslot);
	if (fd && fld_expired(fd, now)) {
		s->expirations++;
		kkv_erase(m, s, r, fslot);
		fd = NULL;
	}
	if (fd) {
		if (parse_i64((const char *)fld_val(fd), fd->vlen, &cur) < 0) {
			rc = DB_ENUM;
			goto done;
		}
		flags = fd->flags;
		/* SET_KEEPTTL is how the router says the request carried
		 * no ttl of its own, so the field keeps the one it had */
		fexp = (o->mode & SET_KEEPTTL) ? fd->expire
		                               : ttl_abs(o->ttl, now);
	} else {
		cur = init;
		flags = o->flags;
		fexp = ttl_abs(o->ttl, now);
	}
	if (__builtin_add_overflow(cur, delta, &next)) {
		rc = DB_ENUM;
		goto done;
	}
	nl = (u32)fmt_i64(num, next);
	if (!(fd = kkv_put(m, s, r, fh, f, fl, nl, &fcreated))) {
		rc = DB_ENOSPC;
		goto done;
	}
	memcpy(fld_val(fd), num, nl);
	fd->expire = fexp;
	fd->flags = flags;
	fd->version = ++kkv_hdr(r)->fseq;
	if (o->set_kttl)
		r->expire = ttl_abs(o->kttl, now);
	r->version = ++s->verseq;
	r->atime = (u32)(now / 1000);
	if (result)
		*result = next;
	fld_meta(meta, fd, now, fcreated);
done:
	shd_unpin(s, 0);
	cont_meta(ci, r, now, created);
	if (rc != DB_OK)
		cont_gc(m, s, r, h, k, kl);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_touch(Db *db, const void *k, u32 kl, const void *f, u32 fl,
             i64 ttl, DbMeta *meta, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, fh, now = now_ms();
	Shard *s;
	Rec *r;
	KkvFld *fd;
	u32 slot, fslot;
	int rc = DB_OK;

	if ((rc = kkv_start(db, k, kl, f, fl)) != DB_OK)
		return rc;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_KKV) {
		rc = DB_ETYPE;
		goto out;
	}
	if (!f) {
		r->expire = ttl_abs(ttl, now);
		r->version = ++s->verseq;
		touch_atime(r, now);
	} else {
		fh = hash_bytes(f, fl, m->seed);
		fd = kkv_find(m, s, kkv_hdr(r), fh, f, fl, &fslot);
		if (fd && fld_expired(fd, now)) {
			s->expirations++;
			kkv_erase(m, s, r, fslot);
			fd = NULL;
		}
		if (!fd) {
			rc = DB_ENOENT;
			cont_gc(m, s, r, h, k, kl);
			goto out;
		}
		fd->expire = ttl_abs(ttl, now);
		fd->version = ++kkv_hdr(r)->fseq;
		fld_meta(meta, fd, now, 0);
		touch_atime(r, now);
	}
	cont_meta(ci, r, now, 0);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_info(Db *db, const void *k, u32 kl, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot)))
		rc = DB_ENOENT;
	else if (r->type != RT_KKV)
		rc = DB_ETYPE;
	else
		cont_meta(ci, r, now, 0);
	lock_release(&s->lock);
	return rc;
}

static int
cont_drop(Db *db, const void *k, u32 kl, u8 type)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if (!(r = live(m, s, h, k, kl, now, &slot)))
		rc = DB_ENOENT;
	else if (r->type != type)
		rc = DB_ETYPE;
	else
		shd_drop(m, s, slot);
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_drop(Db *db, const void *k, u32 kl)
{
	return cont_drop(db, k, kl, RT_KKV);
}

int
db_kkv_scan(Db *db, const void *k, u32 kl, DbFldFn fn, void *arg, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	KkvFld *fd;
	u32 slot, cur = 0, fslot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_KKV) {
		rc = DB_ETYPE;
		goto out;
	}
	shd_pin(s, 0, map_off(m, r));
	while ((fd = kkv_walk(m, s, r, now, &cur, &fslot)) != NULL) {
		DbMeta mm;

		fld_meta(&mm, fd, now, 0);
		if (fn(arg, fld_key(fd), fd->klen, fld_val(fd), fd->vlen, &mm))
			break;
	}
	kkv_compact(m, s, r);
	shd_unpin(s, 0);
	touch_atime(r, now);
	cont_meta(ci, r, now, 0);
	cont_gc(m, s, r, h, k, kl);
out:
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_mset(Db *db, const void *k, u32 kl, const DbItem *it, u32 n,
            const DbKkvOpt *o, u32 *stored, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r = NULL;
	u32 i, done = 0;
	int created = 0, rc;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	for (i = 0; i < n; i++)
		if (!it[i].kl || it[i].kl > m->maxkey || it[i].vl > m->maxval)
			return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if ((rc = cont_open(m, s, h, k, kl, now, RT_KKV, 1, o, &r,
	                    &created)) != DB_OK)
		goto out;
	shd_pin(s, 0, map_off(m, r));
	for (i = 0; i < n; i++) {
		u64 fh = hash_bytes(it[i].k, it[i].kl, m->seed);
		KkvFld *fd;
		int fcreated;

		fd = kkv_put(m, s, r, fh, it[i].k, it[i].kl, it[i].vl,
		             &fcreated);
		if (!fd) {
			rc = DB_ENOSPC;
			break;
		}
		memcpy(fld_val(fd), it[i].v, it[i].vl);
		fd->expire = ttl_abs(it[i].ttl, now);
		fd->flags = o->flags;
		fd->version = ++kkv_hdr(r)->fseq;
		done++;
	}
	if (o->set_kttl)
		r->expire = ttl_abs(o->kttl, now);
	r->version = ++s->verseq;
	r->atime = (u32)(now / 1000);
	shd_unpin(s, 0);
	cont_meta(ci, r, now, created);
	cont_gc(m, s, r, h, k, kl);
out:
	if (stored)
		*stored = done;
	lock_release(&s->lock);
	return rc;
}

int
db_kkv_mdel(Db *db, const void *k, u32 kl, const DbItem *it, u32 n,
            u32 *removed, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot, i, done = 0;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_KKV) {
		rc = DB_ETYPE;
		goto out;
	}
	for (i = 0; i < n; i++) {
		u64 fh;
		u32 fslot;
		KkvFld *fd;

		if (!it[i].kl || it[i].kl > m->maxkey)
			continue;
		fh = hash_bytes(it[i].k, it[i].kl, m->seed);
		if (!(fd = kkv_find(m, s, kkv_hdr(r), fh, it[i].k, it[i].kl,
		                    &fslot)))
			continue;
		kkv_erase(m, s, r, fslot);
		done++;
	}
	if (done) {
		shd_pin(s, 0, map_off(m, r));
		kkv_compact(m, s, r);
		shd_unpin(s, 0);
		r->version = ++s->verseq;
	}
	cont_meta(ci, r, now, 0);
	cont_gc(m, s, r, h, k, kl);
out:
	if (removed)
		*removed = done;
	lock_release(&s->lock);
	return rc;
}

/* ---- queues ----------------------------------------------------------- */

int
db_q_push(Db *db, const void *k, u32 kl, const DbItem *it, u32 n,
          const DbQOpt *o, u32 *stored, DbCont *ci)
{
	Map *m = &db->map;
	DbKkvOpt ko;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r = NULL;
	u32 i, done = 0;
	int created = 0, rc;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	for (i = 0; i < n; i++)
		if (it[i].vl > m->maxval)
			return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);
	memset(&ko, 0, sizeof(ko));
	ko.kttl = o->qttl;
	ko.flags = o->flags;

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if ((rc = cont_open(m, s, h, k, kl, now, RT_QUEUE, 1, &ko, &r,
	                    &created)) != DB_OK)
		goto out;
	shd_pin(s, 0, map_off(m, r));
	q_expire(m, s, r, now, CFG_QEXPIRE_BUDGET);
	for (i = 0; i < n; i++) {
		QEnt *e = q_push(m, s, r, o->right, it[i].vl,
		                 ttl_abs(it[i].ttl, now), o->flags);

		if (!e) {
			rc = DB_ENOSPC;
			break;
		}
		memcpy(e->data, it[i].v, it[i].vl);
		done++;
	}
	if (o->maxlen)
		q_trim(m, s, r, o->maxlen, !o->right);
	if (o->set_qttl)
		r->expire = ttl_abs(o->qttl, now);
	r->version = ++s->verseq;
	r->atime = (u32)(now / 1000);
	shd_unpin(s, 0);
	cont_meta(ci, r, now, created);
	cont_gc(m, s, r, h, k, kl);
out:
	if (stored)
		*stored = done;
	lock_release(&s->lock);
	return rc;
}

/* The shared body of a pop and a peek.  A pop hands an entry over and
 * only then takes it, so a caller that runs out of room leaves the queue
 * exactly as it found it from that entry on. */
static int
q_drain(Db *db, const void *k, u32 kl, u32 max, const DbQOpt *o,
        DbEntFn fn, void *arg, u32 *got, DbCont *ci, int take)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot, n = 0;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_QUEUE) {
		rc = DB_ETYPE;
		goto out;
	}
	shd_pin(s, 0, map_off(m, r));
	q_expire(m, s, r, now, CFG_QEXPIRE_BUDGET);
	if (take) {
		while (n < max) {
			QEnt *e = q_end(m, s, r, o->right);
			DbQMeta em;

			if (!e)
				break;
			if (qent_expired(e, now)) {
				s->expirations++;
				q_take(m, s, r, o->right);
				continue;
			}
			ent_meta(&em, e, now);
			if (fn(arg, e->data, e->vlen, &em))
				break;
			q_take(m, s, r, o->right);
			n++;
		}
		if (n)
			r->version = ++s->verseq;
	} else {
		QIter it;
		QEnt *e;

		q_iter(&it, m, s, r, o->right);
		while (n < max && (e = q_iter_next(m, s, &it)) != NULL) {
			DbQMeta em;

			if (qent_expired(e, now))
				continue;
			ent_meta(&em, e, now);
			if (fn(arg, e->data, e->vlen, &em))
				break;
			n++;
		}
	}
	shd_unpin(s, 0);
	touch_atime(r, now);
	cont_meta(ci, r, now, 0);
	cont_gc(m, s, r, h, k, kl);
out:
	if (got)
		*got = n;
	lock_release(&s->lock);
	return rc;
}

int
db_q_pop(Db *db, const void *k, u32 kl, u32 max, const DbQOpt *o,
         DbEntFn fn, void *arg, u32 *got, DbCont *ci)
{
	return q_drain(db, k, kl, max, o, fn, arg, got, ci, 1);
}

int
db_q_peek(Db *db, const void *k, u32 kl, u32 max, const DbQOpt *o,
          DbEntFn fn, void *arg, u32 *got, DbCont *ci)
{
	return q_drain(db, k, kl, max, o, fn, arg, got, ci, 0);
}

/* Two keys, so two shards - taken in address order, which is the only
 * ordering rule in the store and exists solely for this call.  Both
 * records are pinned because the destination push may evict, and when
 * the two queues share a shard the source is exactly the kind of thing
 * it would find. */
int
db_q_move(Db *db, const void *sk, u32 skl, const void *dk, u32 dkl,
          int from_right, int to_right, const DbQOpt *o, DbEntFn fn,
          void *arg, DbCont *ci)
{
	Map *m = &db->map;
	DbKkvOpt ko;
	u64 hs, hd, now = now_ms();
	Shard *ss, *ds, *first, *second;
	Rec *src, *dst = NULL;
	QEnt *e, *ne;
	DbQMeta em;
	u32 slot;
	int created = 0, rc;

	if (!skl || skl > m->maxkey || !dkl || dkl > m->maxkey)
		return DB_E2BIG;
	hs = hash_bytes(sk, skl, m->seed);
	hd = hash_bytes(dk, dkl, m->seed);
	ss = map_shard(m, hs);
	ds = map_shard(m, hd);
	memset(&ko, 0, sizeof(ko));
	ko.kttl = o->qttl;
	ko.flags = o->flags;

	first = ss < ds ? ss : ds;
	second = ss < ds ? ds : ss;
	lock_acquire(&first->lock);
	if (second != first)
		lock_acquire(&second->lock);
	sweep_some(m, ss);
	if (ds != ss)
		sweep_some(m, ds);

	if (!(src = live(m, ss, hs, sk, skl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (src->type != RT_QUEUE) {
		rc = DB_ETYPE;
		goto out;
	}
	q_expire(m, ss, src, now, CFG_QEXPIRE_BUDGET);
	if (!(e = q_end(m, ss, src, from_right))) {
		rc = DB_ENOENT;
		cont_gc(m, ss, src, hs, sk, skl);
		goto out;
	}
	shd_pin(ss, 0, map_off(m, src));
	if ((rc = cont_open(m, ds, hd, dk, dkl, now, RT_QUEUE, 1, &ko, &dst,
	                    &created)) != DB_OK) {
		shd_unpin(ss, 0);
		goto out;
	}
	shd_pin(ds, 1, map_off(m, dst));

	/* the source entry cannot move: sub blocks are never evicted, and
	 * both records are pinned, so this pointer outlives the push */
	ne = q_push(m, ds, dst, to_right, e->vlen,
	            o->set_ttl ? ttl_abs(o->ttl, now) : e->expire, e->flags);
	if (!ne) {
		rc = DB_ENOSPC;
	} else {
		memcpy(ne->data, e->data, e->vlen);
		ent_meta(&em, e, now);
		q_take(m, ss, src, from_right);
		src->version = ++ss->verseq;
		dst->version = ++ds->verseq;
		dst->atime = (u32)(now / 1000);
		if (fn)
			fn(arg, ne->data, ne->vlen, &em);
	}
	shd_unpin(ds, 1);
	shd_unpin(ss, 0);
	cont_meta(ci, dst, now, created);
	cont_gc(m, ds, dst, hd, dk, dkl);
	cont_gc(m, ss, src, hs, sk, skl);
out:
	if (second != first)
		lock_release(&second->lock);
	lock_release(&first->lock);
	return rc;
}

int
db_q_trim(Db *db, const void *k, u32 kl, u64 maxlen, int right,
          u64 *removed, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms(), n = 0;
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	sweep_some(m, s);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	if (r->type != RT_QUEUE) {
		rc = DB_ETYPE;
		goto out;
	}
	n = q_trim(m, s, r, maxlen, right);
	if (n)
		r->version = ++s->verseq;
	cont_meta(ci, r, now, 0);
	cont_gc(m, s, r, h, k, kl);
out:
	if (removed)
		*removed = n;
	lock_release(&s->lock);
	return rc;
}

int
db_q_touch(Db *db, const void *k, u32 kl, i64 ttl, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
	} else if (r->type != RT_QUEUE) {
		rc = DB_ETYPE;
	} else {
		r->expire = ttl_abs(ttl, now);
		r->version = ++s->verseq;
		touch_atime(r, now);
		cont_meta(ci, r, now, 0);
	}
	lock_release(&s->lock);
	return rc;
}

int
db_q_info(Db *db, const void *k, u32 kl, DbCont *ci)
{
	Map *m = &db->map;
	u64 h, now = now_ms();
	Shard *s;
	Rec *r;
	u32 slot;
	int rc = DB_OK;

	if (!kl || kl > m->maxkey)
		return DB_E2BIG;
	h = hash_bytes(k, kl, m->seed);
	s = map_shard(m, h);

	lock_acquire(&s->lock);
	if (!(r = live(m, s, h, k, kl, now, &slot)))
		rc = DB_ENOENT;
	else if (r->type != RT_QUEUE)
		rc = DB_ETYPE;
	else
		cont_meta(ci, r, now, 0);
	lock_release(&s->lock);
	return rc;
}

int
db_q_drop(Db *db, const void *k, u32 kl)
{
	return cont_drop(db, k, kl, RT_QUEUE);
}
