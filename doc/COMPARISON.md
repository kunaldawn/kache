kache against Redis
===================

Run on one machine, both servers in containers, on 2026-09-16.  The
ratios below are worth something; the absolute numbers are not, and the
Caveats section explains why.  If you are going to quote one line from
this document, quote a caveat.

This is the second run.  The first was taken earlier the same day against
the build before `doc/PERF.md`, and its numbers are kept at the bottom
under "What the first run said", because the difference between the two
is a lesson in itself: almost none of it is the code.

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

Both servers ran in containers with a published port:

    podman run -d --name redis-c -p 7301:6379 redis:7-alpine \
      redis-server --save '' --appendonly no \
      --maxmemory 512mb --maxmemory-policy allkeys-lru

    podman run -d --name kache-c -p 7300:7070 kache:local \
      -f /data/kache.db -s 512M -p 7070 -l 0.0.0.0

Three more kache containers ran alongside, differing only in their
arguments: `-M` (minimal responses), `-t 1` (one worker), and both.  Only
one was ever under load at a time; an idle kache costs nothing.

Redis ran with persistence off, which is how it is normally run as a
cache.  kache has no such mode: it is always writing to a mapped file and
msyncs every second.  That handicap is kache's throughout.

Clients were `redis-benchmark` and `kache-bench`, matched at 8
connections and 8 client threads, 64 byte values, a 100,000 key space,
every store populated first.  **Both clients reached their server through
the published port**, which matters more than it sounds - see below.

Both clients go through the published port
------------------------------------------

The first run's method was ambiguous about this, and the ambiguity was
worth a factor of two.  `redis-benchmark` can be run with `podman exec`
inside the Redis container, where it reaches the server on the
container's own loopback and never touches the rootless port forwarder;
or from a `--network host` container, where it takes exactly the path a
host process takes.  Measured, GET, alternating between the two:

| | in-container | through the published port |
| --- | --- | --- |
| pipeline depth 16 | 598,802 | 598,802 |
| pipeline depth 1 | 119,760 | 59,940 |

At depth 16 the forwarder is free, because its per-message cost is
amortised over sixteen requests.  At depth 1 **it halves Redis's
throughput**, and it would have halved kache's too.  Measuring one server
through it and the other around it would have been the single largest
effect in this document.  Everything below uses the published port for
both, via a long lived `--network host` container:

    podman run -d --name rb-host --network host \
      --entrypoint sleep redis:7-alpine infinity
    podman exec rb-host redis-benchmark -h 127.0.0.1 -p 7301 ...

Method
------

Measurements are **interleaved and rotated**.  Each pass measures all
five targets back to back; the starting point rotates by one each pass,
so no target always sits in the same slot.  Position within a pass was
worth several percent in the first attempt at this run, because the
machine drifts while the pass runs.

The reported ratio is the **median of the per-pass ratios**, not the
ratio of the medians.  Those two are not the same number here, and the
gap is the whole reason for the method: in the GET run Redis dropped from
~599,000 to ~399,000 partway through and stayed there - thermal
throttling on a 15W part - so a column median mixes two machine states
while each pass does not.  Ratio of the medians says 1.43x for pipelined
GET; median of the paired ratios says 1.62x.  The second is the honest
one, because only it compares numbers taken seconds apart.  Absolute
medians appear below for scale only; do not divide them.

Results
-------

Ratios against Redis, medians of per-pass ratios.  Redis is single
threaded by design, so the `-t 1` rows are the honest per core
comparison; the 8 worker rows are the out of the box one.

**Pipelined, depth 16.**

| | GET | SET |
| --- | --- | --- |
| kache, 8 workers | 1.62x | 1.82x |
| kache, 8 workers, `-M` | **1.72x** | 1.82x |
| kache, `-t 1` | 1.62x | 1.86x |
| kache, `-t 1 -M` | **1.81x** | **1.93x** |

**Unpipelined, one request in flight**, which is what an ordinary client
library actually does:

| | single GET | 64 key batch |
| --- | --- | --- |
| kache, 8 workers | 1.07x | **3.06x** |
| kache, 8 workers, `-M` | 1.08x | 3.07x |
| kache, `-t 1` | 1.18x | 1.73x |
| kache, `-t 1 -M` | 1.19x | 1.83x |

kache was ahead of Redis in every pass of every workload: 10/10, 10/10,
9/10 and 8/8 respectively.

Absolute medians, for scale only and not to be divided:

| | Redis | kache, 8 workers |
| --- | --- | --- |
| GET, depth 16 | 574,631 | 822,332 |
| SET, depth 16 | 399,467 | 884,320 |
| GET, depth 1 | 47,923 | 52,026 |
| 64 key batch | 851,914 keys/s | 2,667,607 keys/s |

What the numbers mean
---------------------

**HTTP costs about four times the bytes on a single GET, and `-M` pays
half of that back.**  Measured, for a 64 byte value: kache answers with
201 bytes of headers plus the body, 265 in all; with `-M`, 76 plus the
body, 140.  Redis answers with `$64\r\n`, the body, and `\r\n` - 70
bytes.  That is the price of speaking a protocol every tool already
understands, and `-M` is how much of it is optional rather than
inherent.

**Trimming the headers helps where bytes are the bottleneck, which is
not where you would guess.**  `-M` is worth 1.62x -> 1.72x on pipelined
GET, and 1.07x -> 1.08x unpipelined - nothing.  The intuition that fewer
bytes must help most when each message is a whole round trip is simply
wrong: at depth 1 the cost is the round trip and the thread wakeup, and
125 bytes off a 265 byte response does not move it.  At depth 16 the
server is chewing through messages back to back and bytes are most of
what it is doing.  `doc/PERF.md` has the same result measured on the host
without containers.

**Batching still erases the protocol gap.**  For 64 keys, kache sends 64
frames of `<len>\n<bytes>\n` - 68 bytes each, measured, 4,352 - plus one
set of HTTP headers.  Redis sends `*64\r\n` plus 64 bulk strings of 71
bytes, 4,549.  The header is paid once per batch instead of once per key,
which is also why `-M` does nothing here: it saves the same 39 bytes
whether the batch holds one key or a thousand.

**One worker still beats eight, except where it does not.**  `-t 1` wins
on SET and on unpipelined GET, ties on pipelined GET, and loses badly on
the batch workload (1.73x against 3.06x), which is the one workload with
enough work per request to keep several cores busy.  The first run
concluded flatly that `-t 1` was better; with a quieter machine and a
wider set of workloads that is too simple.  Measure, do not assume - and
measure the workload you actually have.

Caveats
-------

These are load bearing, not disclaimers.

1. **The machine is not a benchmark box.**  A 15W laptop with a desktop
   session on it.  It thermally throttled *during* the GET run, by a
   factor of 1.5, which is why the method pairs everything.  Absolute
   numbers are depressed by perhaps three times against an idle machine.
2. **Two different clients.**  `redis-benchmark` and `kache-bench` are
   both C and both closed loop, but they are not the same program.  Some
   part of every difference here is client, not server.
3. **Redis ran without persistence**, kache with a mapped file and a
   periodic msync.  The comparison is generous to Redis on purpose.
4. **Redis is single threaded on purpose.**  The 8 worker rows are an out
   of the box comparison, not an architectural claim.  The `-t 1` rows
   are the ones to argue from.
5. **One shape of workload**: 64 byte values, 100,000 keys, 100% hit
   rate, no eviction pressure, loopback.  Larger values move the protocol
   overhead ratio; eviction pressure moves everything.
6. **The ratios moved between the two runs and the code is not why.**
   See below.  Do not read two significant figures into any of this.

What the first run said
-----------------------

Earlier the same day, against the build before `doc/PERF.md`:

| | first run | this run | |
| --- | --- | --- | --- |
| GET depth 16, 8 workers | 1.33x | 1.62x | |
| SET depth 16, 8 workers | 1.74x | 1.82x | |
| GET depth 1 | 1.07x | 1.07x | unchanged |
| 64 key batch | 2.04x | 3.06x | |

It is tempting to read that as the work in `doc/PERF.md` paying off.  It
is not, and the evidence is in `doc/PERF.md` itself: measured on the host
against the pre-change binary, the default build - which is what these
rows use, since neither run passed `-M` - came out at **+1.9%, inside the
+1.8% that rebuilding the identical code measured**.  The code did not
get 22% faster at GET.

What changed was the machine and the method: the first run was taken with
a browser and an editor busy in the background, this one on a quieter
machine, and this one rotates the measurement order and pairs the ratios.
The first run's own sixth caveat predicted exactly this - it recorded a
single later pair at 1.78x against its 1.33x median and warned that "if a
change you make moves a ratio by less than a quarter, this rig cannot see
it".  Two of the four rows above moved by about a quarter for no reason
at all.

The only row that is genuinely new is `-M`, which did not exist for the
first run.

Measurements that were thrown away
----------------------------------

Worth recording because the failures are easy to repeat:

- A first, non interleaved pass had **Redis ahead on GET**, 615,006
  against 421,815.  Sequential A-then-B on a drifting machine measures
  the drift.
- Running `podman run --rm ... redis-benchmark` once per pass meant every
  Redis measurement was followed by container teardown and every kache
  measurement began during it.  Switching to a long lived container
  removed a systematic bias worth tens of percent.
- A first attempt at this run measured the five targets in a fixed order,
  Redis always first.  Rotating the order barely touched the targets near
  the front - pipelined GET went 1.61x to 1.62x - but moved the one that
  always sat last, `-t 1`, from 1.55x to 1.62x.  Five percent for nothing
  but position, and it is the last slot that pays it, so a fixed order
  quietly penalises whatever you put at the end.  That is small enough to
  have been missed and large enough to matter.
- The first run's Setup and Reproducing sections described two different
  client paths for `redis-benchmark`, one of which skipped the rootless
  port forwarder.  At depth 1 that is a factor of two, and the
  unpipelined GET row is the one row where such a bias would have changed
  the conclusion rather than the decimal.

A correction carried over from before this document existed: the fair CPU
SET ratio in the first run was 1.80x, not the 2.10x first reported.  That
figure divided the kache median from the four pass run by the Redis
median from the six pass run.  Ratios are only meaningful within one
interleaved run.

Reproducing it
--------------

    podman build -t kache:local -f Containerfile .
    podman pull docker.io/library/redis:7-alpine

    podman run -d --name redis-c -p 7301:6379 docker.io/library/redis:7-alpine \
      redis-server --save '' --appendonly no \
      --maxmemory 512mb --maxmemory-policy allkeys-lru
    podman run -d --name kache-c -p 7300:7070 kache:local \
      -f /data/kache.db -s 512M -p 7070 -l 0.0.0.0
    podman run -d --name kache-m -p 7302:7070 kache:local \
      -f /data/kache.db -s 512M -p 7070 -l 0.0.0.0 -M
    podman run -d --name rb-host --network host \
      --entrypoint sleep docker.io/library/redis:7-alpine infinity

    ./kache-bench -p 7300 -W fill -k 100000 -v 64 -t 8
    ./kache-bench -p 7302 -W fill -k 100000 -v 64 -t 8
    RB="podman exec rb-host redis-benchmark -h 127.0.0.1 -p 7301 \
        --threads 8 -c 8 -P 16 -d 64 -r 100000 -q"
    $RB -n 400000 -t set          # populate, then interleave:
    for i in 1 2 3 4 5 6 7 8 9 10; do
        $RB -n 300000 -t get
        ./kache-bench -p 7300 -W get -t 8 -P 16 -v 64 -k 100000 -d 0.6 -r 1
        ./kache-bench -p 7302 -W get -t 8 -P 16 -v 64 -k 100000 -d 0.6 -r 1
    done

Rotate which of the three goes first on each pass.  Take the ratio within
each pass and then the median of those, not the median of each column.
Distrust any pair whose two halves disagree by more than the pairs around
it.  `redis-benchmark`'s `-q` output is overwritten with carriage
returns, so pipe it through `tr '\r' '\n'` before parsing, and note that
its summary line for a custom command begins with the command text - the
rate is not the second field.
