kache
=====

A multi threaded, in memory, file backed key/value cache with TTLs and
atomic operations, spoken to over HTTP.  Written from scratch in C11,
with no dependencies beyond libc and Linux.

    $ make
    $ ./kache -f /var/tmp/kache.db -s 4G -p 7070
    $ curl -X PUT -d 'hello' 'localhost:7070/kv/greeting?ttl=60'
    $ curl localhost:7070/kv/greeting
    hello

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

Layout
------

    config.def.h     compile time defaults, copied to config.h
    config.mk        compiler and install paths
    src/util/        primitives: types, clock, futex mutex, hash
    src/store/       the mapped file, allocator, index, operations
    src/http/        buffers, parser, router, connections, event loops
    src/main.c       argument handling and startup
    test/test.sh     end to end checks (make check)
    test/micro.c     engine benchmarks, linked against the store
    test/bench.c     HTTP load generator
    test/cmp.c       diffs two benchmark runs
    test/bench.sh    runs the suite and writes one result file
    doc/             design notes, HTTP reference, benchmarking, man page

Each layer only ever includes downwards: `http/` knows about `store/`,
`store/` knows about `util/`, and nothing knows about `http/`.

Design in one page
------------------

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

**Time.**  Expiry is an absolute wall clock millisecond stamp, so TTLs
mean the same thing across a restart.  A ticker thread refreshes a cached
timestamp, and the preformatted HTTP `Date` header with it, so request
handling never enters the clock.

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
    POST   /flush          drop everything (needs -F)
    GET    /stats          counters, one per line
    GET    /metrics        the same, in prometheus form
    GET    /health         liveness

`?ttl=<seconds>` or `?ttlms=<ms>` sets the expiry, `0` meaning never.
`If-None-Match: *` stores only if the key is absent; `If-Match: "<etag>"`
stores or deletes only if the value is still the one you read.  Every
mutation is atomic with respect to every other operation on that key.

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

On a loopback benchmark - 4 worker threads, 8 client threads, 16 deep
pipelining, 64 byte values, 90% reads - this machine served about 2.8M
operations per second, with an unpipelined p50 under 30 microseconds.
Expect that to be bounded by your network long before it is bounded by
the store.

Limits worth knowing
--------------------

It is a cache, not a database.  Writes reach the file when the kernel
decides, or every `-y` milliseconds, or on a clean shutdown; a crash can
lose the most recent ones, which is why recovery validates rather than
trusts.  There is no replication, no key enumeration, and no eviction
notification.  A single process owns a store file at a time, enforced
with an advisory lock.

License
-------

ISC.  See LICENSE.
