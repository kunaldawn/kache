/* kache - the operations.  Each one takes the single lock covering its
 * key, which is what makes it atomic against every other operation. */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "store/alloc.h"
#include "store/db.h"
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

/* Reuse the block in place when it is a decent fit; a much larger block
 * would pin memory the rest of the shard could use. */
static inline int
fits(u32 cap, u32 need)
{
	return cap >= need && (cap <= 128 || need >= cap / 2);
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
	} else if (r->vlen > cap) {
		meta_of(meta, r, now, 0);
		rc = DB_ESMALL;
	} else {
		r->atime = (u32)(now / 1000);
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
install(Map *m, Shard *s, u64 h, const void *k, u32 kl, u32 vl, int creating)
{
	u32 need = REC_NEED(kl, vl);
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
	r = live(m, s, h, k, kl, now, &slot);
	if (r) {
		if (mode & SET_ADD) {
			rc = DB_EEXIST;
			goto out;
		}
		if ((mode & SET_CAS) && r->version != cas) {
			rc = DB_ECAS;
			goto out;
		}
		expire = (mode & SET_KEEPTTL) ? r->expire : ttl_abs(ttl, now);
		if (fits(alc_cap(m, map_off(m, r)), need)) {
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

	if (!(r = install(m, s, h, k, kl, vl, created))) {
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
	r = live(m, s, h, k, kl, now, &slot);
	if (r) {
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

	if (r && fits(alc_cap(m, map_off(m, r)), REC_NEED(kl, nl))) {
		r->vlen = nl;
		memcpy(rec_val(r), num, nl);
	} else {
		if (!(r = install(m, s, h, k, kl, nl, created))) {
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
	if (!(r = live(m, s, h, k, kl, now, &slot))) {
		rc = DB_ENOENT;
		goto out;
	}
	nl = r->vlen + vl;
	if (nl > m->maxval) {
		rc = DB_E2BIG;
		goto out;
	}
	expire = r->expire;
	flags = r->flags;

	if (!prepend && fits(alc_cap(m, map_off(m, r)), REC_NEED(kl, nl))) {
		memcpy(rec_val(r) + r->vlen, v, vl);
		r->vlen = nl;
	} else {
		/* The old value has to survive an allocation that may
		 * evict it, so it is staged outside the mapping first. */
		u32 ol = r->vlen;

		tmp = ol <= sizeof(stack) ? stack : emalloc(ol);
		memcpy(tmp, rec_val(r), ol);
		if (!(r = install(m, s, h, k, kl, nl, 0))) {
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
		lock_release(&s->lock);
	}
}
