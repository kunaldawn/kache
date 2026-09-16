/* kache - the mapped backing file: geometry, open, recover, sync */
#ifndef KACHE_MAP_H
#define KACHE_MAP_H

#include "store/store.h"
#include "util/util.h"

enum {
	KM_FRESH    = 1 << 0,   /* discard any existing contents */
	KM_LOCKED   = 1 << 1,   /* mlock the mapping */
	KM_PREFAULT = 1 << 2,   /* fault the whole file in up front */
	KM_HUGE     = 1 << 3    /* ask for transparent huge pages */
};

typedef struct MapCfg {
	const char *path;
	u64  size;
	u32  shards;
	u32  maxkey;
	u32  maxval;
	u32  avg_item;        /* steers the index/arena split */
	int  flags;
} MapCfg;

typedef struct Map {
	int    fd;
	u8    *base;
	u64    size;
	Hdr   *hdr;
	Shard *shards;
	u32    nshards;
	u32    shard_shift;
	u64    seed;
	u32    maxkey;
	u32    maxval;
	char   path[4096];
} Map;

/* translate a file offset to a pointer and back */
static inline void *
map_at(const Map *m, u64 off)
{
	return m->base + off;
}

static inline u64
map_off(const Map *m, const void *p)
{
	return (u64)((const u8 *)p - m->base);
}

static inline Shard *
map_shard(const Map *m, u64 hash)
{
	/* top bits choose the shard, low bits choose the bucket */
	return &m->shards[hash >> (64 - m->shard_shift)];
}

int  map_open(Map *m, const MapCfg *cfg);
void map_close(Map *m);
int  map_sync(Map *m, int wait);

#endif /* KACHE_MAP_H */
