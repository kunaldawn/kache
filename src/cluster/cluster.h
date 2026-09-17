/* kache - replication across nodes
 *
 * What this is for, in one measurement.  A single hot key answers
 * 15.4M ops/s on one thread in the engine and 4.3M on eight, because
 * every request for it wants the same lock and the same cache line; but
 * over HTTP none of that is visible, because the front end costs several
 * times what the store does.  Serving a hot key entirely out of the per
 * worker set in http/hot.c - removing the store from the path
 * altogether - buys about seven percent.  So per node there is nothing
 * much left to win, and the only thing that multiplies throughput is
 * more nodes.
 *
 * That is unusually easy here, because with one hot key there is no
 * placement problem to solve.  Every node keeps a copy, every read is
 * answered locally, and the nodes only have to agree about writes.
 *
 * Writes have one owner.  The owner of a key is fixed by its hash, so
 * every node computes the same answer without asking anyone, and a
 * write that arrives anywhere else is redirected there.  The owner is
 * therefore the only writer, which is what makes this cheap: replicas
 * receive one ordered stream from one source, so there is no conflict
 * to resolve, no vector clock and no timestamp in the record.  CAS keeps
 * working exactly as it did, because it still happens on one node.
 *
 * Fanout coalesces.  A key written ten thousand times inside one flush
 * window is shipped once, carrying its last value, because the writer
 * records the key and the flusher reads the value at send time.  Peer
 * traffic is therefore set by the flush interval and the number of hot
 * keys, not by the write rate - which is the property that lets a write
 * heavy key survive being replicated at all.
 *
 * The cost is staleness: a write is visible on other nodes after one
 * flush interval plus a round trip.  Reads are always local and never
 * wait for a peer. */
#ifndef KACHE_CLUSTER_H
#define KACHE_CLUSTER_H

#include "store/db.h"
#include "util/util.h"

#define CL_MAX_NODES 64

typedef struct Cluster Cluster;

typedef struct ClusterCfg {
	const char *peers;     /* "host:port,host:port,..." in node order */
	/* What a client should be told to use, when that is not the
	 * address the nodes use between themselves.  Behind NAT, in
	 * containers or under a proxy, the name a node is reachable by
	 * from inside the cluster is not the one a client can resolve, and
	 * a redirect naming the wrong one sends the client nowhere.  Same
	 * order as peers; NULL means they are the same. */
	const char *advertise;
	unsigned    self;      /* this node's index into that list */
	u64         flush_ms;  /* coalescing window, 0 uses CFG_REPL_MS */
} ClusterCfg;

/* Starts the flusher thread.  Returns NULL in *out with 0 when no
 * cluster was asked for, so a caller can ignore the difference. */
int  cl_open(Cluster **out, Db *db, const ClusterCfg *cfg);
void cl_close(Cluster *c);

/* The node that owns this key's writes.  Every node computes the same
 * answer from the hash alone, so ownership needs no agreement and no
 * lookup - it is the top bits of the hash over the node count. */
unsigned cl_owner(const Cluster *c, u64 hash);
int      cl_is_mine(const Cluster *c, u64 hash);
/* "host:port" of a node as the cluster reaches it */
const char *cl_addr(const Cluster *c, unsigned node);
/* the same node as a client should reach it: the advertised address when
 * one was given, otherwise the peer address.  This is what goes in the
 * Location of a redirect. */
const char *cl_client_addr(const Cluster *c, unsigned node);
unsigned    cl_self(const Cluster *c);
unsigned    cl_nodes(const Cluster *c);

/* Note that this node changed a key, so the flusher ships it.  Cheap and
 * coalescing: the key is recorded, the value is read when it is sent. */
void cl_dirty(Cluster *c, const void *k, u32 kl);

/* Apply a batch that arrived from the owner.  Never marks anything
 * dirty, which is what stops a write echoing around the ring for
 * ever. */
int  cl_apply(Cluster *c, const void *body, size_t n, u32 *applied);

/* counters for /stats */
void cl_stats(const Cluster *c, u64 *sent, u64 *recvd, u64 *failed);

#endif /* KACHE_CLUSTER_H */
