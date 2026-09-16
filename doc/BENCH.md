Benchmarking kache
==================

The point of this harness is not to produce a big number.  It is to
answer one question honestly: *did the change I just made help?*

    make baseline          # record the tree as it is now
    ... make your change ...
    make benchmark         # run again and diff against the baseline

Output looks like this:

    metric                                      base           new     delta
    micro_get_hit.ops_per_sec                4431816       4802113     +8.4% better
    micro_evict.ops_per_sec                  1452134       1449002     -0.2%
    http_lat.p99_us                               78            71      -9.0% better

    1 better, 0 worse, 20 unchanged (noise floor 5.0%, widened per scenario)

`make benchmark` exits non-zero if anything regressed, so it can gate a
change without anyone reading the table.

Two tiers, and why
------------------

**`kache-micro` links the engine and calls it directly.**  The HTTP
server tops out on syscalls at a few million operations a second, which
is well past what the store itself costs, so a change to the hash, the
allocator or the eviction path barely moves an HTTP benchmark.  It moves
this one.  If you touched anything under `src/store/` or `src/util/`,
this is the tier that answers you.

**`kache-bench` talks to a real server over a real socket.**  This is
what a user experiences, and it is the tier that notices a change to the
parser, the connection state machine or the event loop.  It is also far
noisier, because it shares the machine with the kernel's network stack
and with whatever else is running.

The numbers from the two tiers are not comparable to each other and are
not meant to be.

How a number is arrived at
--------------------------

Every scenario runs one discarded warmup pass, then `-r` measured
repetitions (three by default).

**Throughput is the best repetition, not the mean or the median.**
Interference is one sided: another process, a migration, a frequency dip
can only ever make a run slower, never faster.  The fastest repetition is
therefore the one that was least disturbed, and the cleanest thing to
compare a later build against.

**Each latency percentile is likewise its lowest observation** across the
repetitions, computed per repetition rather than over all of them merged.
The same argument applies - interference only ever moves a latency up -
and it matters more here, because merging lets one disturbed repetition
set the tail for the whole measurement.  The cost is that p50 and p99 may
come from different repetitions, so they describe the machine's best
behaviour rather than any single distribution.  That is the right trade
for spotting a regression and the wrong one for capacity planning.

**Miss and error rates use the median**, because for those "best" would
just mean flattering.

**The spread across repetitions is reported too**, and it is the honest
error bar.  A scenario that varied by 20% between its own repetitions has
not measured anything, and `kache-cmp` knows it: the threshold for
calling a change real is the larger of the fixed floor (`-t`, 5% by
default) and the spread the two runs themselves showed.  A difference
that is larger than the floor but inside the noise band is marked
`~noise` and not counted.

Two metrics are deliberately reported but not judged.  The 99.9th
percentile is one of them: over a repetition of a second or so it is a
few hundred samples of whatever the machine happened to be doing, and it
moves by more between two identical runs than any plausible code change
would.  It is worth looking at when something else already said there is
a problem, and worth nothing as a gate.  The observed maximum is the same
story, more so.

That still cannot see drift *between* runs - a laptop that was cool for
the baseline and hot for the comparison - so:

Getting numbers worth trusting
------------------------------

- Run on an idle machine.  Close the browser.  The 5% default floor is
  sized for a machine that is merely quiet; on a genuinely idle one the
  engine tier repeats to within about 2%, and `-t 2` will catch more.
- Leave a minute between the baseline and the comparison, or the second
  run measures a warmer CPU.
- Use `-r 5` when a result matters and `-d 2` when it really does.
- Distrust any row whose scenario reported a large `spread`; look at it
  with `kache-cmp -a`.
- Compare only results from the same machine.  `kache-cmp` prints a
  warning when the hostnames differ, because nothing else about the
  comparison would be meaningful.

The result file
---------------

A flat list, one `metric value` per line, plus a `meta.` block recording
the machine, the compiler, the flags and the git revision.  Nothing else
is needed to diff two runs, and nothing about it needs a parser:

    meta.tag baseline
    meta.commit 4f2a91c
    meta.cpu AMD Ryzen 7 PRO 7840U
    micro_get_hit.ops_per_sec 4431816.000
    micro_get_hit.miss_pct 0.000
    micro_get_hit.spread 1.412
    http_lat.p99_us 78.000
    http_lat.p999 245.000

Results live in `bench/` and are not committed: they only mean something
on the machine that produced them.

A run refuses to overwrite an existing result file unless given `-f`, and
refuses to start at all while another run holds the lock in `bench/`,
because two runs appending to one file interleave their metrics into
something no comparison can read.  `kache-cmp` rejects such a file rather
than silently taking whichever value came first, but not starting the
second run is better than diagnosing it afterwards.

A run that is killed outright never reaches its cleanup, so it can leave
a server and a store behind.  The next run notices the stale lock, kills
that server and removes that store before taking over.

The metric naming rule
----------------------

`kache-cmp` has no table of metrics.  A metric's name says which way is
better:

| suffix | meaning |
| --- | --- |
| `_per_sec` | larger is better |
| `_us` `_ns` `_ms` | smaller is better |
| `_per_op` `_pct` | smaller is better |
| anything else | informational: shown with `-a`, never judged |

So adding a measurement to a benchmark needs no change to the comparison
tool, and a measurement that should be reported but not judged - the
thread count, the repetition spread, the arena fill level - simply gets a
name without one of those suffixes.

Scenarios
---------

Engine, via `kache-micro`:

| scenario | what it exercises |
| --- | --- |
| `micro_hash16` `micro_hash64` | the hash alone, on a dependency chain |
| `micro_get_hit` | probe, key compare, value copy |
| `micro_get_miss` | probe that terminates on an empty slot |
| `micro_set_over` | overwrite, the in place path |
| `micro_set_new` | insert into an arena with room |
| `micro_evict` | insert into a full arena: victim choice, coalescing |
| `micro_incr` | parse, add, format, store, under one lock |
| `micro_del_ins` | backward shift deletion and reinsertion |
| `micro_*_mt` | the same at one thread per cpu: lock and memory scaling |

Server, via `kache-bench`:

| scenario | what it exercises |
| --- | --- |
| `http_get` | read path at depth, parser and response building |
| `http_set` | write path at depth |
| `http_mixed` | 90% reads, the shape most caches see |
| `http_incr` | atomic operations end to end |
| `http_lat` | one request in flight: p50, p99, tail |

The store is always populated before the read scenarios run.  A read
benchmark against an empty store measures the miss path, which is both
faster and not what anyone wanted to know.

Running the tools directly
--------------------------

Each tool is usable on its own; `test/bench.sh` only sequences them.

    ./kache-micro -o evict -d 2 -r 5        # one scenario, carefully
    ./kache-micro -k 1000000 -v 1024        # a different shape of store

    ./kache -f /tmp/b.db -s 4G -p 7070 -q &
    ./kache-bench -W fill -k 100000         # populate first
    ./kache-bench -W get -t 16 -P 32 -d 5   # push it
    ./kache-bench -W mixed -L -d 5          # latency instead

    ./kache-cmp -a bench/baseline.txt bench/mine.txt

Comparing against something else
--------------------------------

`doc/COMPARISON.md` is a worked example of all of the above pointed at
Redis rather than at an earlier kache: two servers in containers, matched
client parameters, interleaved measurements, medians, and a section on
the two measurements that came out backwards before the method was fixed.

Finding something other than a regression
-----------------------------------------

The first time this harness ran it found two bugs, which is the argument
for having it.  `micro_evict` hung, because it waits for the arena to
reach nine tenths full and the arena could never get there: the index
saturated at 40% arena use, since the bucket count did not account for
the load factor.  And `http_incr` reported a 100% error rate, because it
was incrementing keys the fill workload had stored strings in.

A benchmark that only ever prints a number cannot tell you either of
those things.  One that fills a store until a condition holds, and
checks the responses it gets back, can.
