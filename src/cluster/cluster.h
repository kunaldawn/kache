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
 * wait for a peer.
 *
 * ---- two modes -------------------------------------------------------
 *
 * The above is CL_REPLICA, reached with -C: a fixed list of nodes, every
 * one holding everything.  It is the right shape when the working set
 * fits on one node and what is wanted is more read throughput.
 *
 * CL_SHARD, reached with -J, is the other shape, and it is the one that
 * belongs under a horizontal autoscaler.  A key lives on its owner and
 * nowhere else, so capacity is the sum of the pods rather than the size
 * of one, and a pod added under memory pressure actually relieves it -
 * which full replication cannot do, because there every new pod holds
 * the whole keyspace again.  Reads redirect like writes, there is no
 * replication stream, and the flusher does not run.
 *
 * Membership is then not a list anyone configures.  Pods under an
 * autoscaler have no stable index and no stable address, so the members
 * are whatever a DNS name resolves to right now - a headless Service in
 * Kubernetes, a service name under Docker Compose, both of which return
 * one A record per live endpoint.  A node finds itself in that list by
 * matching it against its own interfaces, so nothing has to be told
 * which node it is.
 *
 * Ownership therefore has to survive the list changing under it, which
 * rules out the index arithmetic -C can afford: with `hash % nnodes` a
 * 7 to 8 scale event moves 87% of the keyspace, and every moved key is a
 * miss plus a redirect.  Rendezvous hashing over node identity moves the
 * 1/(n+1) that has to move and not one key more.  It is evaluated per
 * bucket when the membership changes, never per request - see the owner
 * table below. */
#ifndef KACHE_CLUSTER_H
#define KACHE_CLUSTER_H

#include "store/db.h"
#include "util/util.h"

#define CL_MAX_NODES 64

enum {
	CL_REPLICA = 0,   /* -C: fixed members, every node holds everything */
	CL_SHARD   = 1    /* -J: discovered members, a key lives on one */
};

typedef struct Cluster Cluster;

typedef struct ClusterCfg {
	int         mode;      /* CL_REPLICA or CL_SHARD */
	/* CL_SHARD: the name whose A records are the members, and the port
	 * they all listen on.  Every pod is given the same one, which is
	 * also what lets them agree on a hash seed without being told. */
	const char *discover;
	const char *port;
	/* This node's own address, when interface matching is not the right
	 * answer.  In Kubernetes that is the downward API's POD_IP; it is
	 * also the escape hatch for a CNI that does not put the pod address
	 * on an interface inside the pod. */
	const char *self_addr;
	u64         resolve_ms;   /* how often to re-resolve; 0 uses the default */
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

/* Where a key belongs.  0 when this node owns it and the caller should
 * just answer; 1 when it does not, with addr filled in with the owner's
 * client facing "host:port" for the Location of a 307.
 *
 * One call rather than an owner index and a second lookup, because under
 * -J the membership can change between two calls and an index resolved
 * against a newer list names a different node.  Everything here is read
 * from one snapshot, so the answer is at worst a moment stale - never
 * self contradictory. */
int cl_route(Cluster *c, u64 hash, char *addr, size_t cap);

unsigned cl_self(const Cluster *c);
unsigned cl_nodes(const Cluster *c);
/* members seen by the last resolve, and how many resolves have happened;
 * both are 0 under -C, where membership never moves */
void cl_discovery(const Cluster *c, u64 *resolves, u64 *changes);

/* Note that this node changed a key, so the flusher ships it.  Cheap and
 * coalescing: the key is recorded, the value is read when it is sent.
 * Does nothing under -J, where there is nothing to ship a copy to. */
void cl_dirty(Cluster *c, const void *k, u32 kl);

/* Apply a batch that arrived from the owner.  Never marks anything
 * dirty, which is what stops a write echoing around the ring for
 * ever. */
int  cl_apply(Cluster *c, const void *body, size_t n, u32 *applied);

/* counters for /stats */
void cl_stats(const Cluster *c, u64 *sent, u64 *recvd, u64 *failed);

#endif /* KACHE_CLUSTER_H */
