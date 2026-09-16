kache against Redis
===================

Run on one machine, both servers in containers, on 2026-09-16.  The
ratios below are worth something; the absolute numbers are not, and the
Caveats section explains why.  If you are going to quote one line from
this document, quote a caveat.

What was compared
-----------------

Only the GET/SET/MGET path.  Redis has sorted sets, streams, pub/sub,
Lua, replication and cluster; kache has none of those and is not trying
to.  This says nothing about whether Redis is the better choice, only
about how fast each one answers the three things they both do.

Setup
-----

| | |
| --- | --- |
| host | Intel i7-10610U, 4 cores / 8 threads, 15W |
| kernel | Linux 7.0.0-31-generic |
| containers | podman 5.7.0, rootless, netavark |
| Redis | `docker.io/library/redis:7-alpine`, v7.4.11, jemalloc 5.3.0 |
| kache | built from `Containerfile` (alpine 3.20, musl) |

Both servers ran in containers with a published port, so both paid the
same rootless network cost:

    podman run -d --name redis-c -p 7301:6379 redis:7-alpine \
      redis-server --save '' --appendonly no \
      --maxmemory 512mb --maxmemory-policy allkeys-lru

    podman run -d --name kache-c -p 7300:7070 kache:local \
      -f /data/kache.db -s 512M -p 7070 -l 0.0.0.0

Redis ran with persistence off, which is how it is normally run as a
cache.  kache has no such mode: it is always writing to a mapped file and
msyncs every second.  That handicap is kache's throughout.

Clients were `redis-benchmark` (from the same image, `--network host`, so
it reached the published port exactly as a host process would) and
`kache-bench`.  Matched at 8 connections and 8 client threads, 64 byte
values, a 100,000 key space, both stores populated first.

Method
------

Measurements were **interleaved**: Redis, then kache, then Redis again,
back to back, repeated.  This machine drifts by a factor of two or three
over the course of a minute, and an interleaved pair shares whatever the
machine was doing, so the ratio survives even when neither number does.
Medians are reported, each against the Redis median from the *same*
interleaved run.

Results
-------

**Pipelined, depth 16.**  Redis is single threaded by design; the third
column gives kache the same single thread, which is the honest per core
comparison.

| | Redis | kache, 8 workers | kache, `-t 1` |
| --- | --- | --- | --- |
| GET | 531,915 | 709,174 — **1.33x** | 834,674 — **1.57x** |
| SET | 399,202 | 693,431 — **1.74x** | 838,138 — **1.80x** |

The `-t 1` column is compared against the Redis medians from its own
interleaved run (532,269 GET and 465,558 SET), not against the column to
its left.

**Unpipelined, one request in flight**, which is what an ordinary client
library actually does:

| | Redis | kache |
| --- | --- | --- |
| single GET | 49,917 | 53,162 — 1.07x |
| 64 key batch | 852,197 keys/s | 1,740,313 keys/s — **2.04x** |

What the numbers mean
---------------------

**HTTP costs about four times the bytes on a single GET.**  Measured,
for a 64 byte value: kache answers with 206 bytes of headers plus the 64
byte body, 270 in all.  Redis answers with `$64\r\n`, the body, and
`\r\n` - 70 bytes.  That is the honest price of speaking a protocol every
tool already understands, and it is why single key GET is a wash rather
than a win.

**Batching erases it.**  For 64 keys, kache sends 64 frames of
`<len>\n<bytes>\n` - 68 bytes each, 4,352 - plus one set of HTTP headers,
about 4,552 bytes.  Redis sends `*64\r\n` plus 64 bulk strings of 71
bytes, 4,549.  Within a tenth of a percent of each other, because the
header is paid once per batch instead of once per key, and a length
prefixed frame is no fatter than RESP.  This is the whole argument for
the batch endpoints: HTTP's overhead is per message, so send fewer, larger
messages.

**kache is faster with one worker thread than with eight.**  On this
machine the load generator wants 8 threads, the server wanted 8 more, and
there are 4 physical cores already about 40% occupied by a desktop
session.  The default of one worker per CPU is counterproductive when
anything else is competing, and `-t 1` was better in every pass.  Worth
knowing before tuning `-t` upward: measure, do not assume.

Caveats
-------

These are load bearing, not disclaimers.

1. **The machine is not a benchmark box.**  A 15W laptop running Chrome
   and VS Code, about 40% of the CPU already spoken for.  Absolute
   numbers are depressed by perhaps three times against an idle machine.
   Interleaving rescues the ratios; nothing rescues the absolutes.
2. **Two different clients.**  `redis-benchmark` and `kache-bench` are
   both C and both closed loop, but they are not the same program.  Some
   part of every difference here is client, not server.
3. **Redis ran without persistence**, kache with a mapped file and a
   periodic msync.  The comparison is generous to Redis on purpose.
4. **Redis is single threaded on purpose.**  The multi threaded column is
   an out of the box comparison, not an architectural claim.  The `-t 1`
   column is the one to argue from.
5. **One shape of workload**: 64 byte values, 100,000 keys, 100% hit
   rate, no eviction pressure, loopback.  Larger values move the protocol
   overhead ratio; eviction pressure moves everything.
6. **Even the ratio moves.**  A single interleaved pair taken later, on a
   quieter machine, put GET at 1.78x rather than the 1.33x median above.
   One pair is not evidence and the median of six stands, but do not read
   two significant figures into any of this.  If a change you make moves
   a ratio by less than a quarter, this rig cannot see it.

Measurements that were thrown away
----------------------------------

Two earlier runs produced the opposite result and were discarded, which
is worth recording because both failures are easy to repeat:

- A first, non interleaved pass had **Redis ahead on GET**, 615,006
  against 421,815.  It was taken while the machine was in a different
  state, and sequential A-then-B on a drifting machine measures the
  drift.
- Running `podman run --rm ... redis-benchmark` once per pass meant every
  Redis measurement was followed by container teardown and every kache
  measurement began during it.  Switching to `podman exec` into a long
  lived container removed a systematic bias worth tens of percent.

A third correction, to a number quoted before this document existed: the
fair CPU SET ratio is 1.80x, not the 2.10x first reported.  That figure
divided the kache median from the four pass run by the Redis median from
the six pass run.  Ratios are only meaningful within one interleaved run.

Reproducing it
--------------

    podman build -t kache:local -f Containerfile .
    podman pull docker.io/library/redis:7-alpine

    podman run -d --name redis-c -p 7301:6379 docker.io/library/redis:7-alpine \
      redis-server --save '' --appendonly no \
      --maxmemory 512mb --maxmemory-policy allkeys-lru
    podman run -d --name kache-c -p 7300:7070 kache:local \
      -f /data/kache.db -s 512M -p 7070 -l 0.0.0.0

    ./kache-bench -p 7300 -W fill -k 100000 -v 64 -t 8
    RB="podman exec redis-c redis-benchmark -h 127.0.0.1 -p 6379 \
        --threads 8 -c 8 -P 16 -d 64 -r 100000 -q"
    $RB -n 400000 -t set          # populate, then interleave:
    for i in 1 2 3 4 5 6; do
        $RB -n 400000 -t get
        ./kache-bench -p 7300 -W get -t 8 -P 16 -v 64 -k 100000 -d 0.6 -r 1
    done

Take the median of each column, and distrust any pair whose two halves
disagree by more than the pairs around them.
