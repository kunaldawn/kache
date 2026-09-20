# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

kache is a multi-threaded, file-backed key/value cache in C11, served over HTTP.
Roughly 11k lines, no dependencies beyond libc and pthreads, Linux-only
(epoll, futex, `accept4`, `MAP_SHARED`).

## Commands

```sh
make                  # build kache and kache-lb
make check            # end-to-end tests (test/test.sh, ~220 checks)
make debug            # rebuild with -fsanitize=address,undefined
make bench            # also build kache-bench, kache-micro, kache-cmp
make benchmark        # run the suite, diff against bench/baseline.txt
make baseline         # record the current tree as the baseline
make clean            # and `make distclean` also removes config.h
```

`config.h` is generated from `config.def.h` on first build and is **gitignored**
(suckless convention). Edit `config.def.h` for a change that should ship; if you
change a default, keep both files in sync or `make distclean` first — a stale
`config.h` silently wins.

### Running one test

`test/test.sh` is a single shell script with no filter — `make check` runs all of
it. To exercise one thing, start a server and curl it:

```sh
./kache -f /tmp/t.db -s 64M -p 17071 -n -q &
curl -i localhost:17071/kv/foo
```

`PORT=17099 sh test/test.sh` moves the suite off the default port. The engine
benchmarks *do* filter: `./kache-micro -o kkv` runs only scenarios whose name
contains `kkv`.

### Benchmarking

Results in `bench/` are only meaningful on the machine that produced them.
Measure A and B **in the same session, interleaved** — a stored baseline from an
earlier session reports phantom regressions on a throttling laptop. Run-to-run
spread is ~1-2.5%; treat anything inside that as noise, and discard the first
measurement (it can land 50%+ above steady state on a cold CPU).

## Architecture

Layers only ever include **downwards**: `http/` → `store/` → `util/`. Nothing
includes `http/`. `cluster/` sits beside the front end and the store stays
unaware of it beyond one handle on `Ctx`. `lb/` links only `util/` — the
balancer never parses a request, which is the point of it.

The headers carry the design rationale; `.c` files carry the mechanism. **Read
the header first** — `store/store.h`, `store/db.h`, `store/kkv.h`,
`store/queue.h`, `http/hot.h`, `cluster/cluster.h` each open with the argument
for why the thing below them looks the way it does.

`doc/DESIGN.md` is the long form. (Its "Things deliberately absent" section still
lists replication as absent; `cluster/` was added after it was written.)

### The store is one mmap'd file

Everything lives in one `MAP_SHARED` file. **Nothing inside the mapping is a
pointer** — every reference is a byte offset from the start of the file, so the
mapping can come back at a different address after a restart. Offset 0 is inside
the header and doubles as the null reference.

Inside a container, references are a `Ref`: the block's offset from *its shard's
arena*, in 16-byte grains, biased by one. See `sub_ref`/`sub_off` in `store.h`.

Layout: `Hdr` → `Shard[nshards]` → per shard `Bucket[nbuckets] | arena`.

### A shard is the unit of locking

One lock covers a shard's index *and* its arena, so a lookup, an allocation and
an eviction need no further synchronisation. Every `db_*` call takes exactly one
shard lock — **except `db_q_move`, which takes two in address order.** That is
the only lock-ordering rule in the codebase; keep it that way.

The lock is a three-state futex mutex (`util/lock.c`), not a rwlock: even a read
stamps the record's access time, so shared acquisition would dirty the same line
anyway.

### Allocation can evict, so `Rec *` is fragile

This is the single easiest thing to get wrong. `shd_alloc` may evict arbitrary
records to make room — **including the one you are holding a pointer to**. Two
established patterns handle it:

- `install()` in `db.c` allocates *first*, then re-looks-up and drops the old
  record, because the lookup it did before allocating may be stale.
- Container operations `shd_pin(s, 0|1, off)` the record across anything that can
  allocate. Sub-blocks are never eviction candidates, so pinning the record
  covers its whole subtree. There are two pin slots because `db_q_move` holds a
  source and a destination that may share a shard.

`db_cat` shows the third case: when the old value must survive an allocation that
may evict it, stage it outside the mapping first.

### Deleting a container is O(1)

Dropping a map of 100k fields or a queue of a million messages does not walk it.
The record leaves the index immediately and joins the shard's **graveyard**
(`s->dead`), flagged `BLK_SUB` so eviction and the forward-coalescing walk ignore
it. It is dismantled a few blocks at a time by `shd_sweep`, called on the way into
any operation that may allocate, and by `db_reclaim` on a once-a-second
housekeeping tick.

### Recovery is a three-pass relayout

Only on unclean shutdown (`ST_CLEAN` absent). `shd_rebuild` runs `relayout` to
make the block chain and index consistent, then `claim()` lets each surviving
container mark what it can still prove it owns, then `relayout` again to free
everything nothing claimed. A cleanly-closed store is trusted as-is and is never
validated — so code that walks a container should not assume its counters agree
with its structure.

### Front end: one epoll loop per thread

Each worker has its own listening socket via `SO_REUSEPORT` — no accept lock, no
handoff queue, no shared connection state. Interest follows the buffers: a
connection is read-interested while it owes no output and write-interested while
it does, which is where pipelining back-pressure comes from for free.

`Buf` has a read cursor; consuming advances it rather than moving bytes.
**`buf_compact` must never be called from inside `buf_grow`** — response building
records absolute indexes into the buffer while a body is assembled (`http_wrap`),
and growing preserves them while compacting would not.

`Stats` and `Hot` are strictly per-worker with no sharing. That is deliberate;
don't add a shared counter to a request path.

The clock is cached (`util/clk.c`): `now_ms()` is an atomic load refreshed by
whichever worker comes out of `epoll_wait` next. No request path calls into time.

### Hot set (`-X`, off by default)

Per-worker cache of *finished responses* for keys that prove hot, answered with a
memcpy and a patched `Date`. Takes no lock and touches nothing another core can
see. A write retires the whole set on that worker (one store, `hot_dirty`) rather
than finding the one entry. An entry never outlives the item's own TTL.

### Cluster: two modes

Both are off by default. They share the `Cluster` handle on `Ctx` and the
`View` snapshot, and differ in everything else.

#### `-C` — replicated, fixed membership

Every node holds a full copy, so any node answers any read locally. Only
**writes** redirect (`307`), from `elsewhere()` calls inside the individual
write handlers. The dirty set coalesces — a key written 10,000 times in one
flush window ships once — so peer traffic scales with hot keys and the flush
interval, not the write rate.

All peer I/O happens on the single flusher thread, so **everything there must be
bounded in time** (`CL_IO_MS`); a peer that stalls must not stop replication to
healthy ones.

#### `-J` — sharded, discovered membership

The autoscaling shape. A key lives on its owner alone, so **reads redirect
too**, and that redirect is emitted from one place: `key_or_fail()`, which every
key-addressed endpoint already calls. Don't add per-handler routing.

Membership is whatever the `-J` DNS name resolves to, re-resolved by a
background thread every `-D` ms. A node identifies itself by matching resolved
addresses against its own interfaces (`-I` overrides).

Three things make this work, and each replaces something `-C` could afford:

- **The seed comes from the cluster's name, not its member list.** Deriving it
  from the list (what `-C` does) means the seed changes when the membership
  does, and then `map_open` refuses the store the node was just serving. Under
  an autoscaler that wipes every cache on every scale event.
- **Ownership is rendezvous hashed into a precomputed bucket table**
  (`own_build`), rebuilt only when the membership actually changes.
  `hash % nnodes` moves ~87% of the keyspace on a 7→8 scale; this moves the
  ~1/(n+1) that has to. The lookup is also *faster* than the modulo it replaced
  — one indexed load vs a runtime division, 0.47ns vs 6.01ns.
- **`View` is an immutable snapshot published by a release store**, flipped
  between two slots exactly like `clk.c` flips its date buffers. Readers take it
  with `view_of()`. This is why `cl_route()` resolves a key to an *address* in
  one call rather than handing back an index to look up separately — an index
  resolved against one membership and an address against the next names the
  wrong node.

A request naming keys on more than one node has no single right answer:
`home_add`/`home_settle` redirect a batch whose keys agree on an owner and
refuse one that spans with `409`. `-X` is refused with `-J`.

### Cluster security

`POST /x/repl` applies peer writes directly, skipping the ownership check, and
nothing identifies the sender. kache has no authentication at all — see the
Security section of README.md. A clustered node's port is not a public port,
and a sharded one is in-cluster only, since its redirects name pod addresses.

### Draining

`-Q ms`: on SIGTERM the server fails `/ready` but keeps serving, then stops.
`/health` stays 200 throughout — liveness says the process works, readiness says
where to send traffic, and they part company only at shutdown. Workers read the
drain flag once per epoll pass into `ctx.draining`.

## Conventions

Suckless-ish C: tabs, 8-column indent, return type on its own line, opening brace
on its own line for functions, `/* */` comments only, no braces on
single-statement bodies. Declarations at the top of a block, not mixed with
statements. Comments explain *why* — match that density rather than narrating the
code.

Warnings are errors in practice: the build runs `-Wall -Wextra -Wshadow
-Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes` and the
tree is clean. Keep it clean.

New parsing code sits in front of untrusted input. Bound every read by the length
you were given — request bodies arrive off a socket and are **not**
NUL-terminated, so `sscanf`, `strlen`, `atoi` and friends read past the request.
`util/util.c` has counted parsers (`parse_u64`, `parse_i64`) for this.

Before changing anything in `store/` or `http/`, run `make debug` and exercise it;
the tree is expected to stay clean under ASan, UBSan and LSan while serving load.
