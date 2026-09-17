/* kache - the key/value interface
 *
 * Every call takes the one shard lock that covers the key, so each
 * operation is atomic with respect to every other one.  Values are
 * copied in and out; nothing hands a caller a pointer into the mapping,
 * which is what makes the lock scopes this short.
 *
 * A key holds one of three things.  A plain value is the db_get / db_set
 * family.  A nested map - a key whose value is itself a key/value store,
 * with a TTL on the outer key and another on every field - is the
 * db_kkv_ family.  A queue is db_q_.  Because a container lives entirely
 * within the shard its outer key hashes to, a multi field write, a whole
 * map enumeration and a move between two queues are each atomic under
 * one lock, with no second level of synchronisation and no lock
 * ordering to get wrong. */
#ifndef KACHE_DB_H
#define KACHE_DB_H

#include "store/map.h"
#include "util/util.h"

enum {
	DB_OK      =  0,
	DB_ENOENT  = -1,   /* no such key */
	DB_EEXIST  = -2,   /* add on a key that is present */
	DB_ECAS    = -3,   /* version did not match */
	DB_ENOSPC  = -4,   /* arena could not make room */
	DB_E2BIG   = -5,   /* key or value over the configured limit */
	DB_ENUM    = -6,   /* value is not a number, or it overflowed */
	DB_ESMALL  = -7,   /* caller's buffer is too small, see meta.vlen */
	DB_ETYPE   = -8    /* the key holds something else */
};

enum {
	SET_ANY     = 0,
	SET_ADD     = 1 << 0,   /* fail if the key exists */
	SET_REPLACE = 1 << 1,   /* fail if it does not */
	SET_CAS     = 1 << 2,   /* fail unless the version matches */
	SET_KEEPTTL = 1 << 3    /* leave an existing expiry alone */
};

/* ttl in milliseconds; negative means the entry never expires */
#define DB_FOREVER (-1)

/* which end of a queue an operation works on */
enum { Q_LEFT = 0, Q_RIGHT = 1 };

typedef struct DbMeta {
	u64 version;
	u32 flags;
	u32 vlen;
	i64 ttl;        /* remaining ms, DB_FOREVER when there is no expiry */
	int created;    /* the call brought the key into being */
} DbMeta;

/* what a container looks like from outside */
typedef struct DbCont {
	u64 version;    /* the container's own CAS token */
	u64 seq;        /* last field version, or last entry id, handed out */
	u64 count;      /* fields, or entries */
	u64 bytes;      /* payload bytes held inside it */
	u32 flags;
	i64 ttl;        /* the container's remaining ms */
	int created;
} DbCont;

typedef struct DbStats {
	u64 keys, bytes, capacity, buckets, inserts, evictions, expirations;
	u64 dead;       /* containers waiting to be reclaimed */
} DbStats;

typedef struct Db {
	Map map;
} Db;

int  db_open(Db *db, const MapCfg *cfg);
void db_close(Db *db);
int  db_sync(Db *db, int wait);

int  db_get(Db *db, const void *k, u32 kl, void *out, u32 cap, DbMeta *meta);
int  db_set(Db *db, const void *k, u32 kl, const void *v, u32 vl,
            i64 ttl, u32 flags, int mode, u64 cas, DbMeta *meta);
int  db_del(Db *db, const void *k, u32 kl, int use_cas, u64 cas);
int  db_incr(Db *db, const void *k, u32 kl, i64 delta, i64 init,
             i64 ttl, int set_ttl, i64 *result, DbMeta *meta);
int  db_cat(Db *db, const void *k, u32 kl, const void *v, u32 vl,
            int prepend, DbMeta *meta);
int  db_touch(Db *db, const void *k, u32 kl, i64 ttl, DbMeta *meta);
void db_flush(Db *db);
void db_stats(Db *db, DbStats *st);
/* Reclaim deleted containers in the background: shards from, from+stride,
 * ... get budget blocks of work each, skipping any whose lock is held.
 * Returns how much was reclaimed. */
u64  db_reclaim(Db *db, u32 from, u32 stride, u32 budget);

/* ---- one item of a batch --------------------------------------------- */

typedef struct DbItem {
	const void *k;     /* field name; unused by the queue calls */
	u32         kl;
	const void *v;
	u32         vl;
	i64         ttl;   /* per field, or per entry */
} DbItem;

/* ---- nested maps ------------------------------------------------------
 *
 * The outer key carries its own TTL and so does every field.  They are
 * independent: a field may outlive nothing in particular and the map may
 * expire with live fields still in it, in which case the whole map goes.
 * A map that loses its last field stops existing, as an empty one has
 * nothing left to say. */

typedef struct DbKkvOpt {
	i64 ttl;         /* the field's, DB_FOREVER for none */
	i64 kttl;        /* the map's */
	int set_kttl;    /* apply kttl; otherwise leave the map's alone */
	u32 flags;
	int mode;        /* SET_* , applied to the field */
	u64 cas;
} DbKkvOpt;

/* Reported for every field an enumeration visits.  Returning non zero
 * stops the walk; the call runs under the shard lock, so it must not
 * come back into the store. */
typedef int (*DbFldFn)(void *arg, const void *f, u32 fl, const void *v,
                       u32 vl, const DbMeta *meta);

int db_kkv_get(Db *db, const void *k, u32 kl, const void *f, u32 fl,
               void *out, u32 cap, DbMeta *meta);
int db_kkv_set(Db *db, const void *k, u32 kl, const void *f, u32 fl,
               const void *v, u32 vl, const DbKkvOpt *o, DbMeta *meta,
               DbCont *ci);
int db_kkv_del(Db *db, const void *k, u32 kl, const void *f, u32 fl,
               int use_cas, u64 cas, DbCont *ci);
int db_kkv_incr(Db *db, const void *k, u32 kl, const void *f, u32 fl,
                i64 delta, i64 init, const DbKkvOpt *o, i64 *result,
                DbMeta *meta, DbCont *ci);
/* f NULL sets the map's own ttl, otherwise the field's */
int db_kkv_touch(Db *db, const void *k, u32 kl, const void *f, u32 fl,
                 i64 ttl, DbMeta *meta, DbCont *ci);
int db_kkv_info(Db *db, const void *k, u32 kl, DbCont *ci);
int db_kkv_drop(Db *db, const void *k, u32 kl);
int db_kkv_scan(Db *db, const void *k, u32 kl, DbFldFn fn, void *arg,
                DbCont *ci);
/* every field of one batch lands under a single hold of the lock */
int db_kkv_mset(Db *db, const void *k, u32 kl, const DbItem *it, u32 n,
                const DbKkvOpt *o, u32 *stored, DbCont *ci);
int db_kkv_mdel(Db *db, const void *k, u32 kl, const DbItem *it, u32 n,
                u32 *removed, DbCont *ci);

/* ---- queues -----------------------------------------------------------
 *
 * A deque with a TTL on the queue and another on every message.  Pushes
 * and pops name an end: Q_LEFT or Q_RIGHT, from queue.h.  Popping the
 * last entry removes the queue, so a drained queue and a queue that was
 * never there answer alike. */

typedef struct DbQOpt {
	i64 ttl;         /* the entry's */
	int set_ttl;     /* the request carried one; only db_q_move reads it */
	i64 qttl;        /* the queue's */
	int set_qttl;
	u32 flags;
	u64 maxlen;      /* trim the far end to this many, 0 = unbounded */
	int right;       /* which end to work on */
} DbQOpt;

typedef struct DbQMeta {
	u64 id;          /* the entry's, unique within the queue */
	u32 vlen;
	u32 flags;
	i64 ttl;
} DbQMeta;

typedef int (*DbEntFn)(void *arg, const void *v, u32 vl, const DbQMeta *e);

int db_q_push(Db *db, const void *k, u32 kl, const DbItem *it, u32 n,
              const DbQOpt *o, u32 *stored, DbCont *ci);
/* Hand at most max entries to fn, taking each one only once fn has it.
 * fn refusing an entry ends the call with that entry still queued. */
int db_q_pop(Db *db, const void *k, u32 kl, u32 max, const DbQOpt *o,
             DbEntFn fn, void *arg, u32 *got, DbCont *ci);
int db_q_peek(Db *db, const void *k, u32 kl, u32 max, const DbQOpt *o,
              DbEntFn fn, void *arg, u32 *got, DbCont *ci);
/* Take one entry off src and put it on dst under both locks at once.
 * The reliable queue primitive: the message is never in neither queue. */
int db_q_move(Db *db, const void *sk, u32 skl, const void *dk, u32 dkl,
              int from_right, int to_right, const DbQOpt *o, DbEntFn fn,
              void *arg, DbCont *ci);
int db_q_trim(Db *db, const void *k, u32 kl, u64 maxlen, int right,
              u64 *removed, DbCont *ci);
int db_q_touch(Db *db, const void *k, u32 kl, i64 ttl, DbCont *ci);
int db_q_info(Db *db, const void *k, u32 kl, DbCont *ci);
int db_q_drop(Db *db, const void *k, u32 kl);

const char *db_strerror(int rc);

#endif /* KACHE_DB_H */
