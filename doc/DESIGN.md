kache internals
===============

This is the long form of the design summary in the README: what the file
looks like, why each structure was chosen, and where the sharp edges are.

The file
--------

One file, mapped `MAP_SHARED`, is the entire store.  There is no separate
in memory representation and no serialisation step, because the live
layout is the on disk layout.

    +----------------------------------------------------+
    | Hdr                                       512 B    |
    | Shard[nshards]                            640 B ea |
    | shard 0: Bucket[nbuckets] | arena                  |
    | shard 1: Bucket[nbuckets] | arena                  |
    | ...                                                |
    +----------------------------------------------------+

Every reference inside the mapping is a byte offset from the start of the
file, never a pointer.  That is what allows the mapping to come back at a
different address after a restart, and it makes offset `0` a free null
sentinel, since offset `0` is inside the header.

The structures are in `src/store/store.h` with `_Static_assert`s on their
sizes, so a change that would silently alter the file format fails to
compile instead.

Geometry
--------

At creation the file is divided into equal slices, one per shard, each an
index array followed by an arena.  The shard count is derived from the
file size unless `-S` says otherwise, and is adjusted until each slice can
hold `CFG_ARENA_ITEMS` maximum sized values.  More shards mean less lock
contention and a smaller arena each; the arena has to stay comfortably
larger than the largest value or a single big item could not be placed.

The split between index and arena comes from `-B`, the arena bytes
provisioned per bucket, default 128.  It only decides which of the two
runs out first.  Too high and the table fills while the arena is half
empty; too low and the index costs memory that stores nothing.  Both
cases still work - whichever fills first starts evicting - so the knob is
about efficiency, not correctness.

Geometry is fixed for the life of the file.  Reopening an existing store
keeps its geometry and says so if the command line disagrees; `-n` starts
over.

Sharding and locking
--------------------

A 64 bit hash (wyhash) selects the shard from its top bits and the bucket
from its low bits, so the two are uncorrelated and a shard's buckets are
not a strided subset of the table.

Each shard has one exclusive lock: a three state futex mutex that spins
`CFG_LOCK_SPINS` times before parking.  A reader/writer lock was the
obvious alternative and is the wrong tool here.  The critical sections are
a probe and a `memcpy`, measured in nanoseconds; a shared lock still
writes to the lock word, so two readers on different cores ping the same
cache line exactly like two writers would, and the extra state machine
buys nothing.  A read also stamps the record's access time for the LRU
approximation, so it is a writer in all but name.  The answer to
contention here is more shards, not a cleverer lock.

Because a shard owns its arena as well as its index, one lock covers the
lookup, the allocation and any eviction that allocation triggers.  There
is no second level of locking anywhere in the store.

The index
---------

Open addressing with linear probing.  A slot is 16 bytes: the full 64 bit
hash and the record's offset, four to a cache line.  Keeping the whole
hash in the slot means a colliding probe is rejected without dereferencing
into the arena at all, which is the difference between a cache miss and
none.  Offset `0` marks an empty slot.

Deletion uses backward shift rather than tombstones: entries following the
hole are pulled back into it unless their own home position would be
skipped over.  The cost is a short loop; the benefit is that a table under
permanent churn - which is the normal state of a cache - never fills with
tombstones and never needs a rehash to recover.

The load factor is capped at `CFG_LOAD_LIMIT/256`, 0.75 by default.  An
insert that would cross it evicts first, so probe runs stay short.

The allocator
-------------

A segregated fit allocator with boundary tags, in `src/store/alloc.c`.
Blocks are multiples of 16 bytes with an 8 byte header holding this
block's size and the previous block's, so both neighbours are reachable in
constant time.  The low bits of the size field are free for flags because
of the alignment.  Free blocks are threaded through their own payload into
64 size bins - exact multiples of 32 below 512 bytes, four bins per power
of two above.  The last 16 bytes of every arena are a permanently in use
sentinel block, which is what makes forward coalescing and forward walks
terminate without a bounds check in the loop.

The reason not to use a slab allocator, which would be simpler and is what
comparable caches use, is calcification.  Fill a slab cache with 100 byte
items and then start writing 4 KiB ones: every page belongs to the small
class, evicting small items yields small chunks, and the large class
starves until something rebalances it.  Coalescing removes the failure
mode instead of managing it - adjacent free blocks merge, so evictions
compound into whatever size is needed next.

Eviction
--------

Victim selection samples `CFG_EVICT_SAMPLES` random slots and takes the
least recently used of them, jumping on anything already expired.  This is
approximated LRU, the same trade Redis makes: no intrusive list to
maintain on the read path, no list head to serialise on, and a quality
that is indistinguishable from true LRU at a sample of eight.  Access time
is a 32 bit second stamp written on every read, which is why the read path
takes the lock exclusively.

One allocation can drive several evictions.  `shd_alloc` retries up to
`CFG_EVICT_ROUNDS` times, and each round does something smarter than
evicting once: it frees the victim, and if the resulting coalesced run is
still too small it walks forward and evicts the neighbours the run would
have to swallow, until the merged block fits.  That is what lets a 60 KiB
value into an arena densely packed with 500 byte records.  Evicting a
record found by arena position rather than by key is possible because a
record stores its own hash, so its index slot can be located by probing.

Expiry
------

Expiry is an absolute wall clock stamp in milliseconds, not a monotonic
one, so a TTL survives a restart with its meaning intact.  It is enforced
lazily: a lookup that finds an expired record drops it and reports a miss,
and the eviction sampler takes expired entries in preference to live ones.
There is no sweeper thread, because a cache under load visits its own
entries far more often than any sweeper could, and one that is idle is not
under memory pressure.

A ticker thread refreshes a cached millisecond timestamp every
`CFG_TICK_MS`, so no request path calls `clock_gettime`.  The same thread
reformats the HTTP `Date` header once a second into one of two buffers and
publishes the index atomically, so a worker copies 29 bytes instead of
calling `gmtime`.

Durability and recovery
-----------------------

Dirty pages reach disk when the kernel decides, when the ticker's periodic
`msync` fires (`-y`, default one second), or on a clean shutdown.  The
header carries a clean flag, cleared on open and set on an orderly close.

If that flag is missing at startup the metadata is treated as suspect,
because a crash could have interrupted an index update or a free list
splice.  What is not suspect is the arena: it is a self describing chain
of blocks.  Recovery walks it, checks each block header for a sane size
and bounds, validates each live record against its own stored hash - which
catches a torn write for free, since the hash was written with the key -
drops the expired and the damaged, rebuilds the index and the free lists
from what is left, and normalises the boundary tags on the way through.  A
shard whose block chain does not add up is reformatted alone; the rest of
the file is unaffected.

The store is a cache, so this is best effort by construction.  The
guarantee is that an unclean restart cannot produce a store that lies:
either a record is intact and indexed, or it is gone.

The front end
-------------

One `epoll` loop per worker thread, each with its own listening socket
opened with `SO_REUSEPORT`.  The kernel load balances new connections
across the listeners, so there is no accept lock, no thundering herd and
no cross thread handoff.  A connection belongs to one thread for its whole
life, so connection state needs no locking at all.

Interest is level triggered, and the rule is one line long: a connection
is registered for reading while it owes no output, and for writing while
it does.  That single rule is also the flow control - a client that
pipelines faster than it reads stops being read from, because the server
is in the writing state - so there is no separate back pressure mechanism
to get wrong.  Level triggering costs one extra wakeup compared to edge
triggering in exchange for removing every drain loop and every "there may
be more data" flag.

The parser copies nothing and allocates nothing: a parsed request is a set
of pointers into the connection's read buffer.  Only the headers the cache
acts on are recognised.  Responses are built body first into the output
buffer and the finished headers are then inserted in front, which costs
one `memmove` of the body and keeps the exact `Content-Length` without
either a second lookup or a padded header.

Both buffers carry a read cursor rather than sliding their contents down
as they are consumed.  That matters most on the input side: a pipelined
batch used to `memmove` the remainder once per request, so draining N
buffered requests was quadratic in N and in their size.  With a cursor the
bytes move at most once per read syscall.  Compaction is therefore
explicit and deliberately absent from `buf_grow`, because response
building holds absolute indexes into the buffer while a body is assembled
and shifting the contents underneath it would invalidate them - growing
may realloc, which preserves them; compacting would not.

Batch operations
----------------

`/mget`, `/mset` and `/mdel` exist for one reason: a `GET` costs about
230 ns in the store and 27 microseconds getting there and back.  For a
caller that needs fifty keys, the round trip is the entire cost, and it is
the only thing worth removing.  HTTP pipelining removes it too, and kache
supports it, but in practice no HTTP client library pipelines - whereas
every one of them can post a list.

The keys in a batch land in different shards and each is taken under its
own lock in turn, so a batch is N independent operations that share a
request rather than a snapshot.  Redis can promise otherwise because it
executes commands on one thread; buying the same promise here would mean
holding several shard locks at once in a fixed order for the length of the
batch, which would serialise exactly the work the sharding exists to
spread.  A cache's callers already cope with a write landing between two
separate `GET`s, so the trade is not a close one.

`/mset` does parse the whole body before writing anything, so a malformed
batch is rejected without changing the store.  A batch cannot be applied
atomically, but it can be rejected atomically, and that is worth the
second pass over a body that is already in memory.

The framing is length prefixed rather than delimited, which is what keeps
values binary safe, and the length of a value is only known once it has
been fetched - so each frame's header is spliced in front of the value
after the fact, the same `buf_insert` the response headers use.

Counters are per worker, one cache line each, written only by their owner.
They are relaxed atomics rather than plain integers: a plain read in
`/stats` next to a plain write in a worker is a data race even though no
real machine would misbehave, and a relaxed load/store pair compiles to
the same instructions while making the program well defined.

Things deliberately absent
--------------------------

*Key enumeration.*  Scanning a shard means holding its lock for the length
of the scan, and a cache that can be scanned invites being used as a
database.

*A binary protocol.*  HTTP costs a few hundred nanoseconds of parsing
against a network round trip, and buys every client library, proxy and
debugging tool that already exists.

*Replication and clustering.*  A cache node that can be lost is a much
simpler thing than one that cannot, and the client already has to handle a
miss.

*Compression and serialisation.*  Values are bytes.  The caller knows what
they mean and kache does not need to.
