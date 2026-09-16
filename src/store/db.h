/* kache - the key/value interface
 *
 * Every call takes the one shard lock that covers the key, so each
 * operation is atomic with respect to every other one.  Values are
 * copied in and out; nothing hands a caller a pointer into the mapping,
 * which is what makes the lock scopes this short. */
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
	DB_ESMALL  = -7    /* caller's buffer is too small, see meta.vlen */
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

typedef struct DbMeta {
	u64 version;
	u32 flags;
	u32 vlen;
	i64 ttl;        /* remaining ms, DB_FOREVER when there is no expiry */
	int created;    /* the call brought the key into being */
} DbMeta;

typedef struct DbStats {
	u64 keys, bytes, capacity, buckets, inserts, evictions, expirations;
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

const char *db_strerror(int rc);

#endif /* KACHE_DB_H */
