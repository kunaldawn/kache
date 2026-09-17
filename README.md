kache
=====

A multi threaded, in memory, file backed cache with TTLs and atomic
operations, spoken to over HTTP.  A key holds a value, a nested map, or a
queue.  Written from scratch in C11, with no dependencies beyond libc and
Linux.

    $ make
    $ ./kache -f /var/tmp/kache.db -s 4G -p 7070

    $ curl -X PUT -d 'hello' 'localhost:7070/kv/greeting?ttl=60'
    $ curl localhost:7070/kv/greeting
    hello

    # a nested map: a day on the key, a minute on one of its fields
    $ curl -X PUT -d alice 'localhost:7070/kkv/user:1?f=name&kttl=86400'
    $ curl -X PUT -d tok42 'localhost:7070/kkv/user:1?f=tok&ttl=60'
    $ curl localhost:7070/kkv/user:1
    4 5 -1
    namealice
    3 5 60
    toktok42

    # a queue, and a worker taking one job without ever losing it
    $ curl -X POST -d 'job payload' localhost:7070/q/work
    $ curl -X POST 'localhost:7070/qmove/work?dst=work:inflight'
    job payload

Why it looks like this
----------------------

The whole store is a single file mapped `MAP_SHARED`.  That one decision
gives three things at once: the cache is in memory because the page cache
holds it, it survives a restart because the kernel writes it back, and it
never needs a serialisation format because the live layout *is* the file
format.  Nothing inside the mapping is a pointer - every reference is a
byte offset - so the file can come back at a different address, or on a
different machine, and still make sense.

The rest follows the suckless line: no configuration language, no plugin
layer, no abstraction that exists to be replaced later.  Tunables live in
`config.def.h`, are compiled in, and are documented where they are
defined.  If a feature is not there, it is because a caller can build it
out of what is.

Hot keys, and more than one node
--------------------------------

One key under heavy load is the case sharding cannot help with: every
request wants the same shard, the same lock and the same cache line.
Measured on this engine, one key on one thread is the *fastest* thing it
does - 4.5x a spread keyspace, because nothing leaves L1 - and the same
key on eight threads is 3.6x slower than on one.  `doc/PERF.md` has the
tables.

Two things address it, and both are off by default.

`-X ms` gives each worker its own small set of finished responses for
keys that have proved hot, answered with a `memcpy` and a patched Date -
no lock, no shard, nothing another core can see.  A key earns its slot
through a doorkeeper, so a uniform keyspace admits nothing and pays
nothing.  The window is how stale a read may be.

    $ kache -X 50

`-C` runs several nodes, each holding a full copy, so reads are answered
locally by whichever node they reach and scale with node count:

    $ kache -f n0.db -p 7070 -C 10.0.0.1:7070,10.0.0.2:7070 -N 0
    $ kache -f n1.db -p 7070 -C 10.0.0.1:7070,10.0.0.2:7070 -N 1

Reads go anywhere - round robin DNS is enough, there is no topology for
a client to learn.  A key's writes belong to one owner, computed from
the hash by every node without asking anyone, and a write that lands
elsewhere gets a 307 naming the owner.  Fanout coalesces on `-Y`, so a
key written 3.8 million times in three seconds was shipped sixty one
times, carrying its latest value each time: **peer traffic follows the
flush interval, not the write rate.**  The cost is that a write is
visible on other nodes after one interval.

If the address nodes use for each other is not the one clients can reach
- containers, NAT, a proxy in front - `-U` gives the client facing
address of each node in the same order, and redirects name that instead.
`docker-compose.yml` is a worked example: three nodes and a balancer, up
with `docker compose up --build -d` and checked with `sh deploy/smoke.sh`.

Every node needs the same `-C` list in the same order.  The hash seed is
derived from that list, so the cluster agrees without configuration, and
a store built under a different list is refused at startup rather than
silently splitting the keyspace.

What is not there yet: only plain keys replicate, so maps and queues
stay node local; `/mset` and `/mdel` are not routed to owners; and a
node that is down is retried on the next flush rather than failed over,
so its keys keep their owner and the writes wait.

kache-lb
--------

`kache-lb` is a connection level balancer for a cluster, built because an
HTTP proxy is the wrong shape in front of this.  nginx does not pipeline
to an upstream: against the same three nodes and the same client it
carried 2,645 ops/s at pipeline depth 16 and 2,560 at depth 64 - no
faster with more pipelining, because it replays the batch one request at
a time.  `kache-lb` carried 501,784 and 1,308,396, which is **190x and
511x**.

It manages that by never parsing anything.  It is L4: accept, pick a
backend, shuttle bytes until somebody hangs up, so a pipelined batch
crosses it as a batch.  That works only because of how the cluster is
built - every node holds a full copy so any node can answer any read,
and a write that lands on a non owner is answered `307`, so the client
goes straight to the owner and never comes back through the balancer.

    $ kache-lb -p 7080 -b 10.0.0.1:7070,10.0.0.2:7070,10.0.0.3:7070

Backends are health checked with `GET /health` once a second and taken
out of rotation while they fail.  Measured at **0.230 syscalls per
request** carried, against kache's own 0.1875, because 16 KiB buffers
coalesce a batch at least as well as the client pipelined it.

A pair that has moved nothing for `-i` seconds is closed, 60 by default
and `-i 0` turns it off.  A balancer holds a slot and a backend
connection for every client it has accepted, so without that a client
that connects and then says nothing costs it both for ever.

One thing to know before benchmarking it: a proxy adds a hop, and at a
fixed number of in-flight requests throughput is concurrency divided by
round trip time.  At depth 16 over two connections it measures 0.55x a
single node - that is the extra hop, not the balancer.  At eight
connections it is 1.07x, and at depth 64 it is 1.28x, because by then it
is spreading the load it exists to spread.  If it looks slow, add
concurrency before blaming it.

Layout
------

    config.def.h     compile time defaults, copied to config.h
    config.mk        compiler and install paths
    src/util/        primitives: types, clock, futex mutex, hash
    src/store/       the mapped file, allocator, index, operations
    src/http/        buffers, parser, router, connections, event loops
    src/cluster/     replication between nodes
    src/lb/          kache-lb, a connection level balancer
    src/main.c       argument handling and startup
    test/test.sh     end to end checks (make check)
    test/micro.c     engine benchmarks, linked against the store
    test/bench.c     HTTP load generator
    test/cmp.c       diffs two benchmark runs
    test/bench.sh    runs the suite and writes one result file
    doc/             design notes, HTTP reference, benchmarking,
                     a measured comparison against Redis, what was
                     profiled and what came of it, man page

Each layer only ever includes downwards: `http/` knows about `store/`,
`store/` knows about `util/`, and nothing knows about `http/`.

Design in one page
------------------

**Containers.**  A nested map and a queue each live entirely inside the
shard their outer key hashes to, so the one lock that covers the key
covers everything in it.  That is what buys atomic multi field writes,
whole map enumeration and a move between two queues - none of which a
flattened `outer\0inner` key space can give you, because there the
pieces land in different shards.  Both levels carry their own TTL, and
they are independent.

**Sharding.**  The key space is split into independent shards, each with
its own lock, index and arena.  The top bits of the hash pick the shard
and the low bits pick the bucket, so the two selections are uncorrelated.
A shard owning its arena is what keeps the locking simple: a lookup, an
allocation and an eviction all happen under one uncontended lock, with
no second level of synchronisation anywhere.

**Locking.**  One exclusive futex backed mutex per shard, spinning
briefly before it parks.  Not a reader/writer lock: the critical sections
are a hash probe and a `memcpy`, and even a read updates the entry's
access time, so a shared lock would dirty the same cache line and cost
more than it saved.  Contention is answered with more shards instead.

**Index.**  Open addressing, linear probing, with the full 64 bit hash
stored beside the offset so a collision is rejected without touching the
record.  Deletion shifts the tail of the probe run back into the hole
rather than leaving a tombstone, so a table that churns forever does not
slowly degrade.

**Allocator.**  A segregated fit allocator with boundary tags in both
directions, living inside the mapping.  Coalescing is the point: because
neighbouring free blocks merge, evicting arbitrary records of any size
eventually yields a contiguous run big enough for the next insert.  A
slab allocator would calcify around whatever item size filled it first.

**Eviction.**  Approximated LRU.  A handful of random slots are sampled,
anything already expired is taken immediately, otherwise the least
recently used of the sample goes.  When the request is for a large block,
eviction continues into the victim's forward neighbours until the merged
run is big enough - which is how a 60 KiB value still gets in when the
arena is a mosaic of 500 byte records.

**Maps.**  Open addressed like the shard index, but the slots are 8
bytes rather than 16 - a small table needs no more than 32 bits of hash
to reject a collision - so eight fit in a cache line and most lookups
touch one.  The tag is the low half of the hash, which is also the
slot's home, so growing rehashes the slot array without reading a single
field record.

**Queues.**  Messages live inside segments rather than in blocks of
their own: one allocation feeds dozens of pushes, and a queue being
drained stays contiguous.  Each entry carries its frame length at both
ends, so a pop from either side is a constant time step - the allocator's
boundary tags, one level up.  Segments start small and double, so a
queue of five messages does not pay for a queue of five million.

**Deferred reclamation.**  Deleting a map of a hundred thousand fields
is a constant time request.  The container leaves the index at once and
is taken apart afterwards, a few blocks at a time, by the traffic that
follows it and by a once a second tick that never waits for a lock.  An
allocation that cannot be met drains that backlog before it evicts
anything a client can still read, so the memory is handed back late,
never lost.

**Time.**  Expiry is an absolute wall clock millisecond stamp, so TTLs
mean the same thing across a restart.  Every worker refreshes a cached
timestamp as its `epoll_wait` returns, and once a second one of them
rebuilds the preformatted HTTP `Date` header with it, so request handling
never enters the clock and no thread exists only to watch it.  Worker 0
carries the periodic `msync` on the same principle.

**Recovery.**  A clean shutdown sets a flag in the header.  If that flag
is missing on open, the index and free lists are not trusted: the arena
is walked block by block, every record is validated against its own
stored hash, expired and damaged ones are dropped, and the index and free
lists are rebuilt from what remains.  The arena is self describing, so
this is both faster and safer than trusting metadata that was being
written when the power went.

**The server.**  One `epoll` loop per thread, each with its own listening
socket opened with `SO_REUSEPORT`, so the kernel spreads connections and
there is no accept lock and no handoff between threads.  Interest is
level triggered and follows the buffers: a connection is read from while
it owes no output and written to while it does, which gives pipelining
back pressure without any explicit flow control.

HTTP interface
--------------

    GET    /kv/<key>       fetch; ETag carries the CAS token
    HEAD   /kv/<key>       metadata only
    PUT    /kv/<key>       store the body
    POST   /kv/<key>       same as PUT
    DELETE /kv/<key>       remove
    POST   /incr/<key>     add ?by=N (default 1) to a numeric value
    POST   /decr/<key>     subtract ?by=N
    POST   /append/<key>   append the body
    POST   /prepend/<key>  prepend the body
    POST   /touch/<key>    reset the ttl
    POST   /mget           many keys, one round trip
    POST   /mset           many values, one round trip
    POST   /mdel           many deletes, one round trip
    POST   /flush          drop everything (needs -F)
    GET    /stats          counters, one per line
    GET    /metrics        the same, in prometheus form
    GET    /health         liveness

    GET    /kkv/<key>              every field, framed
    GET    /kkv/<key>?f=<field>    one field
    PUT    /kkv/<key>?f=<field>    store one field
    POST   /kkv/<key>              many fields, atomically
    DELETE /kkv/<key>[?f=<field>]  one field, or the whole map
    POST   /kkvdel/<key>           many fields, framed names
    POST   /kkvincr/<key>?f=       add ?by=N to a field
    POST   /kkvtouch/<key>[?f=]    a field's ttl, or the map's

    POST   /q/<key>                push the body
    POST   /qpush/<key>            push many, framed
    POST   /qpop/<key>             pop ?n (default 1)
    GET    /q/<key>                the same without removing
    POST   /qmove/<key>?dst=<key>  pop one and push it, under both locks
    POST   /qtrim/<key>?maxlen=    keep at most that many
    POST   /qtouch/<key>           the queue's ttl
    DELETE /q/<key>                remove the queue

`?ttl=<seconds>` or `?ttlms=<ms>` sets the expiry, `0` meaning never.
`?kttl=` sets a container's own expiry, which is independent of the
items inside it.  `?f=<field>` names a field, percent encoded exactly
like a key, because a second path segment could not say where an outer
key containing a slash ends.  `If-None-Match: *` stores only if the key
is absent; `If-Match: "<etag>"` stores or deletes only if the value is
still the one you read, and applies to the field when there is one.
Every mutation is atomic with respect to every other operation on that
key - for a container, that includes the ones touching many of its items
at once.

Anything carrying more than one item is framed by length rather than
delimited, so names and values are binary safe:

    map:   "<fieldlen> <valuelen> [ttl]\n" <field> <value> "\n"
    queue: "<valuelen> [ttl]\n" <value> "\n"

A map's dump is a legal body for a write, so copying one is two
requests.

See `doc/API.md` for the full reference, including status codes.

Tuning
------

Geometry is fixed when the file is created and reopening keeps it; `-n`
starts over.  The knobs that matter:

    -s size    total file size; the cache holds roughly this much
    -V bytes   largest value; drives how many shards fit
    -S n       shards, 0 lets kache choose from the size
    -B bytes   expected average item size; it sizes the index so an
               arena full of items that size can still be addressed
    -t n       worker threads, 0 for one per cpu
    -m         mlock the store so it can never be paged out
    -P         fault the whole file in at startup
    -M         drop the optional response headers; doc/API.md lists
               exactly which, and which are kept regardless
    -A         pin each worker to one cpu

Security
--------

kache has no authentication and no transport security.  Everything below
follows from that, and none of it is a substitute for putting the port
somewhere only the callers you mean can reach it.

Anyone who can open a connection can read and write every key.  `-F`
gates `POST /flush` because dropping the whole store is the one request
that cannot be undone, but it is a guard against a stray script, not
against an attacker - every other write is already unguarded.

`POST /x/repl` is the endpoint peers ship their writes to, and it is the
one that matters most.  It applies writes directly, without the ownership
check that redirects a client to the owner with a 307, because the peer
on the other end is assumed to be the owner.  A request to it from
anywhere else is indistinguishable from one from a peer, so on a node
started with `-C` **anything that can reach the port can write any key on
every node in the cluster**, and those writes propagate.  Nothing about
the request identifies its sender.

So a cluster's port is not a public port.  Bind it to an address only the
peers and your own clients can reach (`-l`), or firewall it, or keep the
cluster on its own network.  If clients have to reach the nodes from
somewhere the peers do not, that is what `-U` and `kache-lb` are for: the
balancer takes the client traffic and the nodes talk to each other
directly.

The request parser is written for untrusted input and is fuzzed as such;
what is deliberately absent is any notion of who is asking.

Building and testing
--------------------

    make                  build
    make check            end to end tests
    make debug            rebuild with address and undefined sanitizers

The test suite covers the store, the conditional writes, the atomics,
expiry, the error paths, and both a clean restart and a `SIGKILL`
recovery.  The tree is clean under `-fsanitize=address,undefined` and
under `-fsanitize=thread` while serving load and scraping `/stats`.

Benchmarking
------------

The harness exists to answer one question: did the change I just made
help?  Record where you are, change something, measure again.

    make baseline         # record the tree as it is now
    make benchmark        # run again and diff against that baseline

    metric                                  base           new     delta
    micro_get_hit.ops_per_sec            4431816       4802113     +8.4% better
    http_lat.p99_us                           78            71      -9.0% better

    1 better, 0 worse, 20 unchanged (noise floor 3.0%, widened per scenario)

`make benchmark` exits non-zero when something regressed, so it can gate
a change without anyone reading the table.

There are two tiers because they answer different questions.
`kache-micro` links the engine and calls it directly, which is the only
way to see a change to the hash, the allocator or eviction - the HTTP
path is bounded by syscalls long before it is bounded by the store.
`kache-bench` drives a real server over a real socket, which is what a
user experiences and what notices a change to the parser or the event
loop.

Throughput is reported as the best of several repetitions rather than
the mean, because interference only ever makes a run slower; the spread
across repetitions comes with it as the error bar, and `kache-cmp`
widens its threshold by exactly that spread before calling anything a
regression.  `doc/BENCH.md` has the rest, including how to get numbers
worth trusting.

On a loopback benchmark - 8 client threads, 64 byte values - this machine
served about 2.8M operations per second at 16 deep pipelining, with an
unpipelined p50 under 30 microseconds.  Expect that to be bounded by your
network long before it is bounded by the store.

The number that matters more, because it is the one an ordinary client
can reach without pipelining:

    single GET, one in flight       284,601 ops/s
    POST /mget,   8 keys          1,458,185 ops/s     5.1x
    POST /mget,  32 keys          3,909,275 ops/s    13.7x
    POST /mget,  64 keys          5,985,996 ops/s    21.0x
    POST /mget, 256 keys          9,414,891 ops/s    33.1x

A `GET` costs about 230 ns in the store and 27 microseconds getting there
and back, so for anything that needs more than one key the round trip is
the whole cost.

The containers are measured the same way, and the engine tier is where
the shape shows.  Per operation, one thread, 64 byte values:

    plain GET                        260 ns
    GET one field of a map           237 ns
    PUT one field of a map           255 ns
    enumerate a map, per field        26 ns     one lock, one walk
    queue push or pop                 88 ns     no allocator, no index
    queue push or pop, batched        14 ns     64 messages a request

A field lookup is two hashes and two probes and still comes out level
with a plain `GET`, because the map and the fields it points at sit
beside each other in the arena while a hundred thousand loose keys do
not.  Reading a whole map costs a tenth of what fetching its fields one
at a time would, and that is the point of the shape.

Over HTTP, 8 client threads at 16 deep, on one run so the numbers are
comparable with each other:

    GET /kv/<key>                2,797,764 ops/s
    GET /kkv/<key>?f=<field>     2,475,361 ops/s
    PUT /kv/<key>                2,477,630 ops/s
    PUT /kkv/<key>?f=<field>     1,483,778 ops/s
    POST /q + POST /qpop         2,407,455 ops/s

The one gap is the field write, and it is not the store: the engine has
the two within four percent of each other.  It is the request saying
more - a longer path, a field to decode, two expiries to look for - and
it is paid in the parser.

Limits worth knowing
--------------------

A container lives in one shard, so it cannot outgrow one shard's arena -
roughly `-s` divided by the shard count, which `/stats` reports as
`bytes_capacity` over `buckets`.  A map or queue that reaches it answers
`507` and stays exactly as it was; the rest of the store is unaffected,
because the other shards were never involved.  Fewer shards with `-S`
buys a larger ceiling per container at the cost of more lock contention,
and that is the whole of the trade.

There is no blocking pop: parking a connection on a queue and waking it
from another worker's thread is a scheduler inside the event loop, built
for one call.  A consumer polls, or holds a connection open and pops a
batch - `?n=64` costs one round trip instead of sixty four.

It is a cache, not a database.  Writes reach the file when the kernel
decides, or every `-y` milliseconds, or on a clean shutdown; a crash can
lose the most recent ones, which is why recovery validates rather than
trusts.  There is no replication, no key enumeration, and no eviction
notification.  A single process owns a store file at a time, enforced
with an advisory lock.

License
-------

ISC.  See LICENSE.
