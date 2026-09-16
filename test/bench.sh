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
EOF
	exit "$1"
}

while getopts 't:o:b:d:r:k:v:T:P:s:qh' opt; do
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

TMP=$(mktemp -d)
STORE="$TMP/bench.db"
pid=
cleanup() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	rm -rf "$TMP"
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
