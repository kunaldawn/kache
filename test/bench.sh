#!/bin/sh
# kache - run the benchmark suite and write one result file.
#
# The file is a flat list of `metric value` lines plus a `meta.` block
# describing the machine and the build, which is everything kache-cmp
# needs to diff one run against another.  Results live in bench/ and are
# not committed: they only mean anything on the machine that produced
# them.
set -u

PORT=${PORT:-17090}
DIR=bench
SIZE=512M
MICRO_SIZE=64M
KEYS=100000
VALSIZE=64
SECS=1
REPS=3
THREADS=8
PIPE=16
TAG=
OUT=
BASE=
QUIET=0
FORCE=0

usage() {
	cat <<'EOF'
usage: bench.sh [-t tag] [-o file] [-b baseline] [-d secs] [-r reps]
                [-k keys] [-v bytes] [-T threads] [-P depth] [-s size] [-q]

  -t tag     name for this run; the default is the git revision or the date
  -o file    write here instead of bench/<tag>.txt
  -b file    compare against this baseline instead of bench/baseline.txt
  -d secs    measured time per repetition          (default 1)
  -r reps    repetitions per scenario, median wins (default 3)
  -k keys    keyspace                              (default 100000)
  -v bytes   value size                            (default 64)
  -T threads client threads for the HTTP tier      (default 8)
  -P depth   pipeline depth                        (default 16)
  -s size    server store size                     (default 512M)
  -q         only print the comparison
  -f         replace an existing result file
EOF
	exit "$1"
}

while getopts 't:o:b:d:r:k:v:T:P:s:qfh' opt; do
	case "$opt" in
	t) TAG=$OPTARG ;;
	o) OUT=$OPTARG ;;
	b) BASE=$OPTARG ;;
	d) SECS=$OPTARG ;;
	r) REPS=$OPTARG ;;
	k) KEYS=$OPTARG ;;
	v) VALSIZE=$OPTARG ;;
	T) THREADS=$OPTARG ;;
	P) PIPE=$OPTARG ;;
	s) SIZE=$OPTARG ;;
	q) QUIET=1 ;;
	f) FORCE=1 ;;
	h) usage 0 ;;
	*) usage 2 ;;
	esac
done

for b in ./kache ./kache-bench ./kache-micro ./kache-cmp; do
	[ -x "$b" ] || { echo "bench.sh: $b is missing, run make bench" >&2
	                 exit 1; }
done

if [ -z "$TAG" ]; then
	# name the run after the revision it measures, or after the clock
	# when there is no repository to ask
	if TAG=$(git rev-parse --short HEAD 2>/dev/null) && [ -n "$TAG" ]; then
		git diff --quiet 2>/dev/null || TAG="$TAG-dirty"
	else
		TAG=$(date -u '+%Y%m%dT%H%M%S')
	fi
fi
[ -n "$OUT" ] || OUT="$DIR/$TAG.txt"
[ -n "$BASE" ] || BASE="$DIR/baseline.txt"
mkdir -p "$DIR"

if [ -e "$OUT" ] && [ "$FORCE" -eq 0 ]; then
	echo "bench.sh: $OUT exists; -f replaces it, -t names another run" >&2
	exit 1
fi

# One run at a time.  Two of them appending to the same result file
# interleave their metrics into something no comparison can read, and
# kache-cmp can only report that afterwards - this stops it happening.
# mkdir is the atomic primitive every shell already has.
LOCK="$DIR/.running"
if ! mkdir "$LOCK" 2>/dev/null; then
	old=$(cat "$LOCK/pid" 2>/dev/null)
	if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
		echo "bench.sh: a run is already going (pid $old)" >&2
		exit 1
	fi
	# A run killed outright never reached its trap, so it may also
	# have left a server and a store behind.  Clear up after it.
	srv=$(cat "$LOCK/server" 2>/dev/null)
	if [ -n "$srv" ] && [ "$(cat /proc/"$srv"/comm 2>/dev/null)" = kache ]
	then
		echo "bench.sh: killing a server left by pid ${old:-?}" >&2
		kill "$srv" 2>/dev/null
	fi
	stale=$(cat "$LOCK/tmp" 2>/dev/null)
	case "$stale" in
	/tmp/tmp.*) rm -rf "$stale" ;;
	esac
	echo "bench.sh: clearing a stale lock from pid ${old:-?}" >&2
	rm -f "$LOCK"/*
fi
echo $$ > "$LOCK/pid"

TMP=$(mktemp -d)
STORE="$TMP/bench.db"
echo "$TMP" > "$LOCK/tmp"
pid=
cleanup() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	rm -rf "$TMP" "$LOCK"
}
trap cleanup EXIT INT TERM

say() { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }

# ---- metadata ---------------------------------------------------------
{
	echo "# kache benchmark result"
	echo "meta.tag $TAG"
	echo "meta.date $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	echo "meta.commit $(git rev-parse --short HEAD 2>/dev/null || echo none)"
	echo "meta.host $(uname -n)"
	echo "meta.kernel $(uname -sr)"
	echo "meta.cpu $(sed -n 's/^model name[ \t]*: //p' /proc/cpuinfo |
	                head -1)"
	echo "meta.cores $(getconf _NPROCESSORS_ONLN)"
	echo "meta.cc $($(sed -n 's/^CC *= *//p' config.mk | head -1) --version 2>/dev/null |
	                head -1)"
	echo "meta.cflags $(sed -n 's/^CFLAGS_OPT *= *//p' config.mk | head -1)"
	echo "meta.params keys=$KEYS value=$VALSIZE secs=$SECS reps=$REPS threads=$THREADS pipeline=$PIPE"
} > "$OUT"

# ---- the engine, in process -------------------------------------------
say "engine:"
./kache-micro -m -f "$TMP/micro.db" -s "$MICRO_SIZE" -k "$KEYS" \
	-v "$VALSIZE" -d "$SECS" -r "$REPS" >> "$OUT" || exit 1
[ "$QUIET" -eq 1 ] || sed -n 's/^\(micro_[a-z0-9_]*\)\.ops_per_sec /\1 /p' "$OUT" |
	while read -r n v; do printf '  %-18s %12.0f ops/s\n' "$n" "$v"; done

# ---- the server, over a socket ----------------------------------------
./kache -f "$STORE" -s "$SIZE" -p "$PORT" -q -n &
pid=$!
echo "$pid" > "$LOCK/server"
i=0
while ! curl -sS -m 1 -o /dev/null "http://127.0.0.1:$PORT/health" 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 50 ] && { echo "bench.sh: server did not start" >&2; exit 1; }
	sleep 0.1
done

# Populate first: a read benchmark against an empty store measures the
# miss path and nothing else, and would not be comparable between runs.
./kache-bench -p "$PORT" -W fill -k "$KEYS" -v "$VALSIZE" -t "$THREADS" \
	>/dev/null

say "server:"
run() {
	name=$1
	shift
	./kache-bench -m -n "$name" -p "$PORT" -k "$KEYS" -v "$VALSIZE" \
		-d "$SECS" -r "$REPS" "$@" >> "$OUT"
	[ "$QUIET" -eq 1 ] && return
	# the latency scenario reports percentiles instead of a rate
	rate=$(sed -n "s/^$name\.ops_per_sec //p" "$OUT")
	if [ -n "$rate" ]; then
		printf '  %-18s %12.0f ops/s\n' "$name" "$rate"
	else
		printf '  %-18s %12s p50 %.0fus  p99 %.0fus\n' "$name" "" \
			"$(sed -n "s/^$name\.p50_us //p" "$OUT")" \
			"$(sed -n "s/^$name\.p99_us //p" "$OUT")"
	fi
}
run http_get   -W get   -t "$THREADS" -P "$PIPE"
run http_set   -W set   -t "$THREADS" -P "$PIPE"
run http_mixed -W mixed -t "$THREADS" -P "$PIPE" -R 90
run http_incr  -W incr  -t "$THREADS" -P "$PIPE" -k 10000
# deep pipeline with larger values: the shape where per-request buffer
# handling costs more than the store does
./kache-bench -m -n http_deep -p "$PORT" -k 20000 -v 1024 -d "$SECS" \
	-r "$REPS" -W set -t "$THREADS" -P 64 >> "$OUT"
[ "$QUIET" -eq 1 ] || printf '  %-18s %12.0f ops/s\n' http_deep \
	"$(sed -n 's/^http_deep\.ops_per_sec //p' "$OUT")"
# what an ordinary, non pipelining client gets from a batch: one
# request in flight, sixty four keys in it
./kache-bench -m -n http_mget -p "$PORT" -k "$KEYS" -v "$VALSIZE" \
	-d "$SECS" -r "$REPS" -W mget -t "$THREADS" -P 1 -B 64 >> "$OUT"
[ "$QUIET" -eq 1 ] || printf '  %-18s %12.0f ops/s\n' http_mget \
	"$(sed -n 's/^http_mget\.ops_per_sec //p' "$OUT")"
run http_lat   -W mixed -t "$THREADS" -L -R 90

# what the store looked like at the end, for context when a number moves
curl -sS "http://127.0.0.1:$PORT/stats" |
	sed -n 's/^\(keys\|bytes_used\|bytes_capacity\|evictions_total\|expirations_total\) /store.\1 /p' \
	>> "$OUT"

kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
pid=

say ""
say "wrote $OUT"

# ---- compare ----------------------------------------------------------
echo
if [ "$BASE" = "$OUT" ]; then
	echo "recorded as the baseline; future runs are measured against it"
elif [ -f "$BASE" ]; then
	./kache-cmp "$BASE" "$OUT"
else
	echo "no baseline at $BASE; record one with: make baseline"
fi
