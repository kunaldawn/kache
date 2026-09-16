Profiling kache
===============

Taken on 2026-09-16, against commit `de2e839`, on one laptop.  It
records where a request's time actually goes on that machine, which
changes were made because of it, and - at least as usefully - which
ideas were measured and dropped.

Every number here belongs to the machine and the workload that produced
it, and most of them belong to a particular pipeline depth as well.  A
figure quoted without its depth is not a figure.  If you are going to
take one line out of this document, take the conditions with it.

The machine
-----------

| | |
| --- | --- |
| host | Intel Core i7-10610U, 4 cores / 8 threads, 15W, 8MB L3 |
| kernel | Linux 7.0.0-31-generic |
| state | an ordinary desktop session, browser and editor open |
| store | files on `/tmp`, which is tmpfs on this host |

This is not a benchmark box, for the same reasons `doc/COMPARISON.md`
gives.  The absolutes are depressed; the paired ratios survive.

How the numbers were taken
--------------------------

**The usual tools were not available.**  `perf` is off
(`kernel.perf_event_paranoid = 4`), there is no valgrind, and yama's
`ptrace_scope = 1` means `strace` can only attach to a child it spawned
itself.  Nothing here came from a sampling profiler, so nothing here is
a flat profile of where instructions retire.  What stood in:

- `strace -c` on a server started *as a child of strace*, with the
  request count read back from `/stats`, for syscalls per request.
- `/proc/<pid>/stat`, `utime + stime` over a fixed window, for server
  CPU per request.
- the in-process `kache-micro` harness for anything below the socket,
  because that is the only tier that can see it.

**The noise floor, before any result.**  A paired interleaved A/B at the
socket level on this machine has a spread of about ±15% per pair.  A
control - the identical code compiled twice, run through the same
pairing - measured **+1.8% over 7 pairs**.  So at the socket level
nothing below roughly 10% is resolvable at all, and a single-digit win
there is indistinguishable from the rebuild.  The micro harness is
stable to ±0.2% on `micro_get_mt` and resolves about 1%.

That is why the socket-level results below are reported as medians of
paired runs with the control's own number beside them, and why several
changes in this set carry no number of their own.

The profile
-----------

**Syscalls per request**, `strace -c` on a child server, request count
from `/stats`:

| depth | requests | read | write | epoll_wait | per request |
| --- | --- | --- | --- | --- | --- |
| P=1 | 94,255 | 94,264 | 94,257 | 47,279 | 1.000 + 1.000 + 0.502 = **2.50** |
| P=16 | 1,678,961 | 104,945 | 104,938 | 104,962 | 0.0625 each = **0.1875** |

Pipelining amortises the syscalls 16x, exactly as the arithmetic says it
should.  Unpipelined, every request pays two syscalls and a trip through
`epoll_wait` on every other request.

**Server CPU per request**, from `/proc/<pid>/stat` over a 4s window
with 2 workers:

| P=1 | P=4 | P=16 | P=64 |
| --- | --- | --- | --- |
| 5.32us | 1.68us | 1.11us | 0.537us |

Extrapolating that curve, the per-*event* cost - what the server pays
once per wakeup rather than once per request - is about **4.8us**.  Two
syscalls are perhaps 0.3us of it; this CPU has no KPTI
(`/sys/devices/system/cpu/vulnerabilities/meltdown` reads "Not
affected"), so the syscalls really are cheap here and would not be on a
mitigated host.  The rest is the thread blocking and being woken.  At
P=1 the server spends most of its CPU not on the request.

**The engine against its working set**, `kache-micro`, `micro_get_hit`,
1GB store, 4 threads:

| keys | ops/s | per op |
| --- | --- | --- |
| 20,000 | 8,487,424 | 118ns |
| 200,000 | 3,338,883 | 300ns |
| 1,000,000 | 3,223,228 | 310ns |
| 4,000,000 | 2,963,315 | 337ns |

Once the index stops fitting in the 8MB L3, about 60% of the GET path is
memory stalls.  A change to the instruction count of the lookup is
therefore worth very little on a large store and quite a lot on a small
one, which is the opposite of the intuition.

**What the shard lock costs.**  Measured by stubbing `lock_acquire` and
`lock_release` to nothing.  That is *incorrect code* - it does not
implement anything, it only removes the work - so it bounds the prize
rather than measuring a fix.  Medians of 3:

| scenario | locked | stubbed | |
| --- | --- | --- | --- |
| `micro_get_hit`, 1 thread | 3,651,742 | 3,759,143 | +2.9% |
| `micro_get_mt`, 4 threads | 11,819,527 | 13,463,483 | +13.9% |

The spreads on those two `micro_get_mt` figures were 0.06% and 0.2%, so
the 13.9% is real at that tier even though it is far below what the
socket tier could see.  Nobody gets all of it; a seqlock read would
claim some fraction.

**The idle server** measured 2% CPU before this change set, entirely the
1ms ticker thread.  That is a before figure.  No matching after figure
was taken, so do not quote one - what can be said is that the thread
which accounted for it no longer exists.

What changed, and what each change measured
-------------------------------------------

**Minimal response mode (`-M`, `CFG_MINIMAL`).**  A response then omits
`Server: kache`, omits `Connection: keep-alive` when the connection is
in fact being kept alive, and on a successful GET of `/kv/<key>` only,
omits `Content-Type` and the `ETag` / `X-Kache-TTL` / `X-Kache-Flags`
trio.  A HEAD of the same key keeps them - it returns no bytes, so the
elision would save nothing.  Writes keep all of it, so CAS and conditional
requests still work under `-M`, and an HTTP/1.0 client is never answered
minimally because it needs the `Connection: keep-alive` echo.

7 pairs, GET at **pipeline depth 16**, 4 client threads against 4
workers, store prefilled with 100,000 64-byte values, **on this
machine**:

| | median ops/s | |
| --- | --- | --- |
| base | 1,337,521 | |
| control, the same code rebuilt | 1,361,247 | +1.8% |
| minimal headers | 1,498,946 | **+12.1% over base** |

So: **+12.1% at depth 16 on this machine, against a control that
measured +1.8%.**  Minimal won 6 of the 7 pairs; the pair it lost was
the first, which is the one that runs before the machine has thermally
settled.  Nothing about that number transfers to a different depth, a
different value size, or a host whose network is the bottleneck - the
saving is bytes, and bytes matter in proportion to how much of the work
is bytes.

That was the prototype.  The shipped implementation was measured again
the same way, three-way against the pre-change binary over 8 pairs:

| | median ops/s | |
| --- | --- | --- |
| pre-change binary | 1,456,085 | |
| shipped, default | 1,484,463 | +1.9% |
| shipped, `-M` | 1,654,751 | **+13.6% over pre-change** |

`-M` won 6 of the 8 pairs, losing only the two hot opening passes.  The
second row is the one worth dwelling on: the default path, carrying
every other change in the set, lands at +1.9% - inside the +1.8% the
control measured.  That is the expected answer, not a disappointing
one.  None of the smaller changes was ever going to be visible through
a +-15% socket-level spread, and the point of measuring the default
path was to show that removing the ticker thread, coarsening the
activity list, eliding the atime write and rewriting the two formatters
cost nothing.  They did not.

Response size for a 64-byte value, measured on the shipped
implementation with `curl`: header bytes **201 -> 76**, whole response
**265 -> 140**.  Both halves came from one measurement, against a key
whose `ETag` was `"1"` - a longer version number makes the `ETag` line
longer and the saving slightly larger.  `doc/COMPARISON.md` counts the
same 201 and 265 against Redis's 70 bytes for the same value.

**The ticker thread is gone.**  Every worker now calls `clk_update()`
right after `epoll_wait` returns and before it reads `now_ms()`, so the
cached clock is refreshed by whoever needed it.  `clk_update()` was made
safe for N concurrent callers: `clk_ms` is published monotonically with
a CAS loop (a reading far enough behind is either a long stall or the
clock being set, and one more read of the clock tells those apart), and
the once-a-second `Date` rebuild is handed to exactly one worker by a
try-lock, so the two-buffer scheme keeps the single writer it was
designed around.  A worker that does not get the lock has nothing to
contribute and returns; nobody ever waits.  The periodic `msync` moved to worker 0 with its *own*
deadline rather than being nested in the reap tick, which would have
rounded `-y` up to a second.  `CFG_TICK_MS` is deleted.

What this measured is the 2% idle CPU above, and one fewer thread to
schedule.  It is not a throughput change and was not measured as one.

**`conn_touch` coarsened by `CFG_TOUCH_MS` (1000ms).**  An active
connection is no longer relinked to the young end of the idle list on
every event.  The skip path deliberately does *not* update `c->atime`
either: `reap_idle` stops at the first connection newer than the cutoff,
so a fresh `atime` at a stale list position would hide every idle
connection behind it and leak descriptors.  The consequence is that a
connection can be reaped up to `CFG_TOUCH_MS` early, never late.  A
`_Static_assert` keeps `CFG_TOUCH_MS * 4 <= CFG_IDLE_MS`, and `main.c`
`die()`s if `-i` sets a runtime idle timeout that low.

Not measured on its own.  It removes two list writes per event, which is
below every floor available here.

**atime elision in `db_get`**, gated on `CFG_ATIME_SLACK` (4 seconds),
with the comparison written as unsigned so a record stamped ahead of the
clock is restamped rather than frozen.  Writes still stamp
unconditionally.  Measured alone in the micro harness:

| scenario | before | after | |
| --- | --- | --- | --- |
| `micro_get_hit` | 3,642,077 | 3,639,615 | no change |
| `micro_get_mt` | 11,915,874 | 12,063,214 | +1.2% |

`micro_get_hit` does not move because the record's line is already being
touched by the read, so the extra store is nearly free.  The reason to
keep the change is not the 1.2%: it is that a read no longer dirties an
mmap page, which is what `msync` and writeback pay for.  That second
effect is the argument, and it is not what these two numbers measure.

**Three smaller changes that were not measured individually.**  The
X-macro status-line table in `http.c` (a known code becomes one `memcpy`
of a compile-time length instead of `lit()` plus `fmt_u64()` plus a
runtime `strlen`; unknown codes fall back to the old path), the
`take_key` fast path (`memchr` for `%` first, and when absent a bounds
check plus `memcpy` instead of `url_decode`'s per-byte loop, with the
length test kept *inside* the no-escape branch because an escaped key
may be up to `KEYMAX*3` raw bytes and still decode to a legal one), and
the `fmt_u64` rewrite (two digits per division from a 200-entry table,
built back-to-front into a 20-byte scratch and `memcpy`'d out, so it
never touches `dst[20]`).

These three were applied together with the header trimming and are below
the socket-level noise floor on their own.  There is no number for any of
them and none should be implied.  What can be said is what they cost to
have: nothing.  Each keeps a fallback for the case it does not handle,
each was verified for correctness rather than for speed - `fmt_u64` and
`fmt_i64` byte-identical to `printf` over 0..200000 exhaustively, over 3
million random u64, and at 0/9/10/99/100/UINT64_MAX/INT64_MIN with no
write past the returned length; `take_key` byte-identical in status code
against the pre-change binary over a battery of 255-, 256-, 766- and
765-byte keys, `%`, `%z`, `%zz`, `a%`, `a%2`, `a%2Fb%20c`, empty, `x/y`,
`nul%00byte` and every method - and so the case for them is that they
are not worse and are on the path that the profile says is hot.

**Worker count and affinity.**  `ncores()` was added to `util.c`; it
counts the cpus that lead their own `thread_siblings_list`.  `nworkers`
is now `cfg->threads ? cfg->threads : (CFG_THREADS_SMT ? ncpu() :
ncores())`, and `CFG_AFFINITY` pins worker `i` to cpu `i % ncpu()`.
Both knobs default to today's behaviour (`CFG_THREADS_SMT 1`,
`CFG_AFFINITY 0`), so nothing changed unless you change it.  Neither was
measured directly.  The reason the thread knob exists is in
`doc/COMPARISON.md`: on this machine `-t 1` beats the default on SET and
on unpipelined GET and ties on pipelined GET - but it loses badly on the
batch workload, 1.73x against 3.06x, which is the one with enough work
per request to keep several cores busy.  That is an argument for a knob,
not for a different default.

**A pre-existing bug, fixed in passing.**  `server_run()`'s `out:`
cleanup walks all `nworkers`, but only the workers the setup loop
reached had their `lfd`/`epfd`/`wfd` set to -1; the rest were 0 from
`ecalloc`, so a mid-setup failure closed fd 0 up to three times per
unreached worker.  All descriptors are now set to -1 up front.  Not a
performance change and it has no number.

Every new knob defaults to today's behaviour: `CFG_MINIMAL 0`,
`CFG_AFFINITY 0`, `CFG_THREADS_SMT 1`.  `CFG_ATIME_SLACK 0` and
`CFG_TOUCH_MS 0` each restore the unconditional path.

What was tried and rejected
---------------------------

**`-march=native` plus `-flto`.**  9 paired passes, median ratio
**1.0224**, i.e. +2.2%.  The control for this rig is +1.8%, so +2.2% is
inside the noise band and this is a negative result.  It is recorded as
one.  Two percent might well be there; this machine cannot say so, and
the cost - a binary that only runs on the machine that built it - is
paid whether or not the two percent is real.

**Huge pages, on this host.**  The size of the prize was measured
directly, with a random pointer chase over anonymous memory,
`MADV_HUGEPAGE` against `MADV_NOHUGEPAGE`, verified through
`AnonHugePages` in `/proc/self/smaps_rollup`:

| working set | 4K pages | huge pages | |
| --- | --- | --- | --- |
| 8MB | 64.1ns | 70.3ns | worse; it already fits in L3 |
| 64MB | 115.5ns | 108.8ns | |
| 256MB | 128.6ns | 108.8ns | +18% |
| 512MB | 130.7ns | 92.1ns | +42% |

But on **this** host huge pages are not available to the store's
`MAP_SHARED` file mapping at all:
`/sys/kernel/mm/transparent_hugepage/enabled` is
`always [madvise] never`, `shmem_enabled` is
`always within_size advise [never] deny force`, and `HugePages_Total` is
0.  So the existing `-H` flag is a no-op here, and returns success
regardless - which is its own small bug, listed below.  The table sizes
what a differently configured host would gain; it is not a kache result.

What is still on the table
--------------------------

Each of these was sized by the profile above and none of them is in this
change set.

- **io_uring.**  At P=1 the server pays 2.50 syscalls and one block/wake
  per request, about 4.8us of CPU per event.  Multishot recv with a
  provided buffer ring takes both to roughly nothing.  At P=16 the
  syscalls are already amortised 16x, so expect little there.  Needs a
  runtime-detected fallback: Docker's default seccomp profile has
  blocked io_uring since 2023.
- **Adaptive spin-poll**, the cheap half of the same idea: a budget of
  non-blocking `epoll_wait` calls, refilled whenever a poll returns
  events.  Prototyped, not shipped.  At **P=1 with 8 client
  connections**, base median 183,112 -> 196,964 ops/s (**+7.6%**),
  winning 5 of 6 pairs, and an idle server still measured the same 2%
  CPU.  That figure understates it: the load generator shares the same 4
  cores as the server, so this rig penalises anything that trades CPU
  for latency.  About 15 lines of code.
- **Optimistic (seqlock) reads**, to stop a GET writing the shard lock
  word twice and pulling the line exclusive.  Bounded above at +13.9% on
  4 threads by the stubbed-lock measurement, which is a ceiling and not
  a forecast.
- **Huge pages**, worth 18-42% of the memory stall on a large store by
  the table above, but needing either a root sysctl plus a tmpfs store,
  or `memfd_create` with `MFD_HUGETLB`.  While in there, `-H` should
  verify that it got what it asked for instead of silently succeeding.
- **Batch lookups with memory-level parallelism.**  `do_mget` calls
  `db_get` in a loop, which is N serial dependent miss chains.  Hashing
  all the keys first, prefetching every bucket and then every record
  overlaps them, and grouping by shard takes each lock once instead of
  once per key.  The working-set table is the argument: on a large store
  those chains are most of the cost.
- **Pre-rendered responses**: keep the finished HTTP response beside the
  value in the arena, rebuilt on write, and patch the 29 date bytes in
  place on the way out.  The front end is roughly two thirds of
  per-request CPU at P=16.
- **A co-located client** that maps the store read-only and does lookups
  in its own address space: no socket, no syscall, no copy.  Needs the
  seqlock first.

Reproducing the measurements
----------------------------

All of it from the repository root, on an otherwise idle machine, with
the caveats of `doc/BENCH.md` applying throughout.

Syscalls per request - `strace` must *spawn* the server, because
`ptrace_scope` will not let it attach to one:

    strace -f -c -o /tmp/sc.txt ./kache -f /tmp/p.db -s 1G -p 7070 -q &
    ./kache-bench -p 7070 -W fill -k 100000 -v 64
    ./kache-bench -p 7070 -W get -t 4 -P 16 -v 64 -k 100000 -d 4 -r 1
    curl -s localhost:7070/stats          # request count for the divisor
    kill %1; cat /tmp/sc.txt

Server CPU per request - fields 14 and 15 of `/proc/<pid>/stat` are
`utime` and `stime` in clock ticks; take them before and after a fixed
window and divide by the requests `/stats` gained over the same window:

    awk '{print $14, $15}' /proc/$(pgrep -n kache)/stat

Engine tier, one scenario at a time, with the store shaped explicitly:

    ./kache-micro -o get_hit -s 1G -k 20000  -v 64 -t 4 -d 2 -r 3
    ./kache-micro -o get_hit -s 1G -k 4000000 -v 64 -t 4 -d 2 -r 3
    ./kache-micro -o get_mt  -s 1G -k 100000 -v 64 -t 4 -d 2 -r 3

Socket tier, paired and interleaved, which is the only way any of the
socket numbers above mean anything.  Run the two servers from two
builds, alternating, and take the median of the per-pair ratios:

    for i in 1 2 3 4 5 6 7; do
        ./kache.base -f /tmp/a.db -s 1G -p 7070 -t 4 -q & sleep 1
        ./kache-bench -p 7070 -W get -t 4 -P 16 -v 64 -k 100000 -d 2 -r 1
        kill %1
        ./kache.new  -f /tmp/b.db -s 1G -p 7071 -t 4 -M -q & sleep 1
        ./kache-bench -p 7071 -W get -t 4 -P 16 -v 64 -k 100000 -d 2 -r 1
        kill %1
    done

Fill each store before the read pass.  Discard the first pair if the
machine was cold - it is the one that behaves differently, and saying so
afterwards is not the same as deciding so beforehand.  **Run the control
first**: build the same source twice and pair those two binaries against
each other.  If the control does not come back near +1.8% on this
machine, the rig is measuring something other than the code, and no
result from that session should be quoted.

Response size, on the shipped binary:

    curl -sS -D - -o /dev/null localhost:7070/kv/k | wc -c

Huge page availability on the host, before believing any `-H` result:

    cat /sys/kernel/mm/transparent_hugepage/enabled
    cat /sys/kernel/mm/transparent_hugepage/shmem_enabled
    grep HugePages_Total /proc/meminfo
