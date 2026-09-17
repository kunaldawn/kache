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
buys nothing.  A read writes as well - it drops a record it finds
expired, and it stamps the record's access time for the LRU
approximation - so it is a writer in all but name.  The answer to
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
is a 32 bit second stamp, which is why the read path takes the lock
exclusively.

A read only rewrites that stamp once it has fallen `CFG_ATIME_SLACK`
seconds behind.  The sampler never compares an atime against the clock,
only against the other seven samples, so a few seconds of lag cannot
change which of them looks oldest - while stamping every hit dirties the
record's cache line and the mapped page under it, which is how a pure
read ends up handing the next `msync` a page to write back.  The
comparison is unsigned, so a record stamped ahead of the clock is
restamped on the next read rather than frozen until the clock catches
up.

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

There is no timekeeping thread either.  A cached millisecond timestamp
stands in for `clock_gettime` on every request path, and each worker
refreshes it by calling `clk_update` the moment `epoll_wait` returns,
before it reads the clock.  The cache is therefore maintained by whoever
has work to do: under load it is fresher than a thread ticking every
millisecond ever made it, and with the server idle it is at most one
epoll timeout - 200 ms - stale, which nothing can observe because
nothing is asking.  With N publishers instead of one, the timestamp is
moved forward with a compare and swap loop rather than stored, so a
worker preempted between reading the clock and publishing it cannot drag
the cache backwards over a newer value; a jump back of more than a
second is the clock actually being set, and that is followed.

The same call maintains the preformatted HTTP `Date` header, so a worker
copies 29 bytes instead of calling `gmtime`.  The once a second rebuild
is claimed rather than scheduled: every worker crosses the second
boundary at about the same moment, and a compare and swap on a flag
hands the rebuild to exactly one of them.  The rest have nothing to
contribute and go straight back to work, so nobody ever waits on it.
That restores the single writer the two buffer scheme was designed
around: the holder formats into whichever buffer is not published and
then publishes the index, so a reader sees either the previous second or
the current one and never a half written string.  Handing the buffer out
by any scheme that lets two rebuilds overlap does not work - a stalled
formatter would be writing the buffer a reader is holding.

Durability and recovery
-----------------------

Dirty pages reach disk when the kernel decides, when the periodic
`msync` fires (`-y`, default one second), or on a clean shutdown.  With
no separate thread to carry that flush, worker 0 runs it on a deadline of
its own rather than folding it into the idle sweep it already performs,
which would silently round `-y` up to the sweep's interval.  The
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

Hot keys
--------

Sharding answers contention by spreading keys over more locks, which
does nothing at all when the load is on one key: every request wants the
same shard, the same lock and the same cache line.  Measured, that is
not a small effect in either direction - one key on one thread is the
fastest thing the engine does, 4.5x a spread keyspace because nothing
ever leaves L1, and the same key on eight threads is 3.6x slower than on
one.  `doc/PERF.md` has the table.

So the hot path stops sharing instead of sharing better.  Each worker
keeps a small set of *finished responses* - headers and body, exactly as
they would go out - for the keys that have proved hot, in memory no
other thread touches.  A hot GET copies one out and patches the 29 byte
Date.  No lock is taken, no line is pulled away from another core, and
the answer costs the same on eight workers as on one.

Not every key may be kept, and not every response.  A key has to earn a
slot: a doorkeeper of one byte per hash bucket, cleared once a second,
admits a key only after `CFG_HOT_ADMIT` sightings inside one window.
Without it a uniform keyspace would evict the set on every request and
pay the copy for keys nobody asks for twice - measured, a 100,000 key
workload admits nothing and costs nothing.  The response has to be one
that can be replayed byte for byte, so a HEAD, a conditional request, an
HTTP/1.0 client and a connection being closed all fall through to the
ordinary path.

The cost is staleness bounded by `CFG_HOT_MS`.  A write retires the
whole set of the worker that took it - a generation counter, so it is
one store rather than a hash of the written key - and a connection
belongs to one worker for its whole life, so a client still reads its
own writes.  What it cannot see immediately is somebody else's.

Replication
-----------

The same measurement that justifies the hot set also bounds it: with
99.8% of requests answered without reaching the store, throughput moves
by seven percent.  The front end is the rest, so a node is worth roughly
what it is worth, and more throughput means more nodes.

With one hot key there is no placement problem, which is what makes this
small.  Every node keeps a copy, every read is answered locally by
whichever node it arrives at, and nothing is partitioned.  There is no
slot map, no consistent hashing, no migration and no topology for a
client to learn - a client load balances by any means it likes,
including round robin DNS.

Writes are the only thing the nodes have to agree about, and they agree
by not sharing.  A key's writes belong to one owner, and the owner is
the top bits of the key's hash over the node count - so every node
computes the same answer from the key alone, with no lookup and nothing
to keep in sync.  A write arriving anywhere else is answered with a 307
naming the owner.  It is a redirect and not a forward on purpose:
forwarding would have one node holding a request open while it waits on
another, which is how a slow peer becomes this node's queue.

Because the owner is the only writer, a replica receives one stream from
one source over one connection, in order.  That is the whole conflict
resolution story - there is no vector clock, no timestamp in the record
and no merge rule, and `Rec` did not have to grow a field.  CAS is
unaffected, because it still happens on one node.  The owner's version
does travel on the wire, but only as something to look at in a tcpdump:
a replica assigns versions from its own shard counter, so the two
numbers are not comparable and the code says so where it ignores it.

Fanout coalesces, and that is the property that makes a *write* hot key
replicable rather than merely a read hot one.  A worker taking a write
records the key; the flusher reads the value when it sends.  So a key
written 3.8 million times inside three seconds is shipped sixty one
times - once per `-Y` interval - each time carrying the value it ended
up with.  Peer traffic is O(hot keys / interval) and owes nothing to the
write rate.

A deletion has to travel in its own right, as a tombstone, precisely
because the value is read at send time: a key that was deleted is simply
not there any more, and without a frame saying so the peers would keep
serving the copy they already had.

Two things every node must share.  The member list, in the same order,
since ownership is computed from it; and the hash seed, since ownership
is computed from the hash.  A store seeds itself at random, so `-C`
derives the seed from the member list - the cluster agrees without
anyone configuring it, and a store created under a different list is
refused at startup instead of quietly serving a keyspace its peers
disagree about.

What this does not do yet is listed honestly in README.md: containers
are not replicated, batch writes are not routed, and a node that is down
is retried rather than failed over.

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

Idle connections are found without scanning for them.  Each worker keeps
its connections in a list ordered by activity, youngest at the tail, and
the sweep walks from the head and stops at the first one newer than the
cutoff.  Moving a connection to the tail on every event would pay six
pointer writes across three cache lines to feed a sweep that runs once a
second, so the move is coarsened to at most once per `CFG_TOUCH_MS`.
When the move is skipped the activity stamp is deliberately left stale
too: a fresh time at an old list position would hide every genuinely
idle connection behind it, and their descriptors would leak.  The price
is paid in accuracy instead - a connection can be closed up to
`CFG_TOUCH_MS` early, never late - which is why the idle timeout has to
stay well clear of it, checked at compile time and again against `-i`.

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

Containers
----------

A key holds one of three things, named by a type byte in the record: a
value, a nested map, or a queue.  The two container types keep a fixed
size control block where a plain value would sit, and everything hanging
off it - slot tables, field records, queue segments - is an ordinary
arena block carrying the `BLK_SUB` flag.  Sub blocks are not in the
index: they are reachable only from their container.

That one decision is what makes the rest cheap.  A container lives
entirely inside the shard its outer key hashes to, so the lock that
covers the key covers everything in it, and a read, a write, an
enumeration and a many field update are each atomic against every other
operation on that key with no second level of synchronisation anywhere.
A flattened `outer\0inner` key space gets none of that: the pieces of
one map land in different shards, so there is no enumeration, no atomic
multi field write, and no way to delete the map except by knowing every
field's name.

The price of that is a ceiling: a container cannot outgrow the arena of
the one shard it lives in, which is the file divided by the shard count.
Reaching it is a `507` and nothing else - the container is exactly as it
was, and the other shards never heard about it.  Spreading one container
across shards would buy a bigger ceiling and cost the property the whole
design is built on, so the knob is `-S` instead.

There is a second, smaller price.  Eviction grows a free run by
swallowing the blocks in front of it, and it cannot swallow a sub block,
because a sub block is not a key and cannot be evicted on its own.  So a
shard densely packed with container data assembles a large contiguous
run by evicting whole containers rather than by merging forwards, which
takes more rounds to arrive at the same place.

Inside the mapping a container refers to its blocks by a 32 bit *ref*:
the block's offset from the start of its shard's arena, in grains,
biased by one so zero stays the null reference.  An arena is at most 4
GiB and blocks are 16 byte aligned, so 32 bits is more than enough,
which halves the size of a slot table and of a segment header.

A container's control block is a row of 64 bit counters and has to be 8
byte aligned, so the key in front of it is padded up to the next
multiple of eight.  The key still comes first, because the index
compares against it without knowing what kind of key it is.

The nested map
--------------

Open addressed with linear probing, like the shard index, but its slots
are half the size: 8 bytes of 32 bit tag and 32 bit ref, eight to a cache
line, so most lookups touch one line.  The tag is the *low* 32 bits of
the field hash, not the high ones, and that is deliberate - the low bits
are also the slot's home position, which is what backward shift deletion
needs in order to know whether an entry may move.  A 32 bit tag still
rejects a collision without dereferencing into the arena.

Growing allocates a new table and rehashes, and the rehash reads the old
slot array and nothing else: the tag carries the whole of the new
position, so not one field record is touched.  Shrinking happens when a
map falls to an eighth of its table, but never during an enumeration -
an enumeration erases expired fields as it meets them, and the slot
array must not move underneath it.

A field is shaped like a record and validated the same way, with its own
hash, expiry, CAS token and flags, plus a back reference to the map that
owns it.  Field and map expiries are independent: either can outlive the
other, and a map whose own TTL lapses takes its live fields with it.

The queue
---------

A queue could have been a linked list of records - one allocator call,
one index slot and a 56 byte record per message.  Instead entries live
inside *segments*: an arena block holding a run of framed entries,
filled from whichever end is being pushed.  One allocation feeds dozens
of pushes, the bytes of a queue being drained stay contiguous, and the
index never learns that the queue has more than one key in it.

Each entry carries its frame length at both ends, so a pop from either
side is a constant time step - the same boundary tag idea the arena
allocator uses one level down.  Overhead is 32 bytes and a rounding to
eight, against 56 plus an index slot plus an allocator call for the
record per message version.

Segments start at `CFG_QSEG_MIN` and double up to `CFG_QSEG_MAX`, so a
queue of five messages does not pay for a queue of five million and a
queue of five million does not allocate every few pushes.  A message too
large for the ceiling gets a segment of its own.  An empty segment is
freed unless it is small and the only one, which is what keeps a queue
that hovers around empty from allocating on every message; because it is
empty it can be re-aimed at either end for nothing.

`qmove` is the only call in kache that holds two locks.  It takes them
in address order, pins both records, builds the destination entry and
only then takes the source one, so the message is in one queue or the
other and never in neither - which is what makes it usable as the
reliable queue primitive it looks like.

Pinning
-------

An allocation inside a container may evict, and the one thing eviction
must not take is the container being walked.  Sub blocks are never
eviction candidates to begin with, so pinning the container's record
covers the whole subtree hanging off it: the sampler skips a pinned
slot, and the forward coalescing walk stops at one.  There are two pin
slots per shard because `qmove` holds a source and a destination, and
they may share a shard.

In use blocks never move in this allocator, which is the invariant the
whole scheme rests on: a pointer into a pinned container stays valid
across an allocation that evicts half the shard around it.

Deferred reclamation
--------------------

Deleting a map of a hundred thousand fields would otherwise be one
caller's problem - a single request holding the shard lock while it
walks a structure the size of the arena.  Instead the container leaves
the index at once and joins a per shard graveyard, threaded through the
control blocks of the dead containers themselves, and is taken apart a
few blocks at a time by the requests that follow it and by a once a
second housekeeping tick on each worker.  The tick takes the shard lock
with a try and never waits for it: a shard that is busy is one whose own
traffic is already sweeping it.

A dead container is flagged `BLK_SUB`, which by then is exactly true of
it - it is no longer in the index - and that is what keeps eviction and
the forward coalescing walk from looking at it again.

The memory is handed back late, not lost.  An allocation that cannot be
satisfied sweeps, retries, drains the whole graveyard, and only then
starts evicting things a client can still read, so a store under
pressure never chooses eviction over reclamation.

Recovering a container
----------------------

Recovery of plain records is a single forward walk of a self describing
block chain.  Containers make it two sided: a block flagged `BLK_SUB`
says it belongs to one, but not that the container still wants it - the
pointer that did could have been half written.  So the walk runs three
times.

The first pass normalises the block chain, keeping every sound top level
record and every sub block, and rebuilds the index from the records.
The second walks out from each surviving container and validates what it
claims to own: bounds, alignment, the owner back reference, the sizes,
each field against its own stored hash, each queue segment's entry chain
against its boundary tags and its own count.  Claiming is part of
validating - a block already marked belongs to somebody else, so a stale
pointer cannot make two containers share one field - and a container
that fails any of it is simply unindexed and left unmarked.  The third
pass lays the arena out again, keeping only what was marked.

A map's probe runs are checked too, in one pass rather than by probing
from every home: start at an empty slot and walk, and inside each run of
occupied slots every home must lie between the run's start and the slot
itself.  That is exactly the linear probing invariant, and a table that
breaks it would answer a lookup with a miss for a field that is there.

The guarantee is the one the plain records already had, extended: an
unclean restart cannot produce a store that lies.  Either a container is
intact with everything it points at, or it is gone.


Things deliberately absent
--------------------------

*Key enumeration.*  Scanning a shard means holding its lock for the length
of the scan, and a cache that can be scanned invites being used as a
database.  Enumerating one nested map is a different thing and is
supported: it is bounded by that map's size, which its owner chose.

*Blocking pops.*  A `BLPOP` would have to park a connection on a queue,
wake it from another worker's thread, and unpark it on a timeout, which
is a scheduler inside the event loop for one call's benefit.  A consumer
polls, or holds a connection open and pops a batch.

*A binary protocol.*  HTTP costs a few hundred nanoseconds of parsing
against a network round trip, and buys every client library, proxy and
debugging tool that already exists.

*Replication and clustering.*  A cache node that can be lost is a much
simpler thing than one that cannot, and the client already has to handle a
miss.

*Compression and serialisation.*  Values are bytes.  The caller knows what
they mean and kache does not need to.
