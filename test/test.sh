#!/bin/sh
# kache - end to end checks.  Run with `make check`.
set -u

PORT=${PORT:-17071}
DIR=$(mktemp -d)
BIN=./kache
URL="http://127.0.0.1:$PORT"
fails=0
pid=
pid2=
pid3=
pid4=
pid5=

cleanup() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	[ -n "$pid2" ] && kill "$pid2" 2>/dev/null
	wait "$pid2" 2>/dev/null
	[ -n "$pid3" ] && kill "$pid3" 2>/dev/null
	wait "$pid3" 2>/dev/null
	[ -n "$pid4" ] && kill "$pid4" 2>/dev/null
	wait "$pid4" 2>/dev/null
	[ -n "$pid5" ] && kill "$pid5" 2>/dev/null
	wait "$pid5" 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

ok() {
	printf '  ok    %s\n' "$1"
}

bad() {
	printf '  FAIL  %s: %s\n' "$1" "$2"
	fails=$((fails + 1))
}

# check NAME EXPECTED curl-args...
check() {
	name=$1 want=$2
	shift 2
	got=$(curl -sS -m 10 -o "$DIR/body" -w '%{http_code}' "$@")
	if [ "$got" = "$want" ]; then
		ok "$name"
	else
		bad "$name" "expected $want, got $got"
	fi
}

# body NAME EXPECTED curl-args...
body() {
	name=$1 want=$2
	shift 2
	got=$(curl -sS -m 10 "$@")
	if [ "$got" = "$want" ]; then
		ok "$name"
	else
		bad "$name" "expected '$want', got '$got'"
	fi
}

hdr() {
	curl -sS -m 10 -D - -o /dev/null "$@" | tr -d '\r'
}

# has NAME HEADERS PATTERN - the header block must carry the line
has() {
	if printf '%s\n' "$2" | grep -q "$3"; then
		ok "$1"
	else
		bad "$1" "no line matching $3"
	fi
}

# lacks NAME HEADERS PATTERN - and must not
lacks() {
	if printf '%s\n' "$2" | grep -q "$3"; then
		bad "$1" "unexpected line matching $3"
	else
		ok "$1"
	fi
}

[ -x "$BIN" ] || { echo "build kache first"; exit 1; }

"$BIN" -f "$DIR/test.db" -s 64M -p "$PORT" -F -q >"$DIR/log" 2>&1 &
pid=$!
i=0
while ! curl -sS -m 1 -o /dev/null "$URL/health" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "server did not start"; cat "$DIR/log"; exit 1; }
	sleep 0.1
done

echo "basics"
check "health"            200 "$URL/health"
check "index"             200 "$URL/"
check "unknown endpoint"  404 "$URL/nothing"
check "miss"              404 "$URL/kv/absent"
check "delete a miss"     404 -X DELETE "$URL/kv/absent"

echo "store and fetch"
check "put"               201 -X PUT -d hello "$URL/kv/a"
body  "get"               hello "$URL/kv/a"
check "overwrite"         204 -X PUT -d world "$URL/kv/a"
body  "get overwritten"   world "$URL/kv/a"
check "head"              200 -I "$URL/kv/a"
check "post is put"       201 -X POST -d v "$URL/kv/b"
check "delete"            204 -X DELETE "$URL/kv/b"
check "gone"              404 "$URL/kv/b"
body  "key with slashes"  deep "$(curl -sS -o /dev/null -X PUT -d deep "$URL/kv/x/y/z"; echo "$URL/kv/x/y/z")"
check "percent escapes"   201 -X PUT -d esc "$URL/kv/a%20b%2Fc"
body  "escaped readback"  esc "$URL/kv/a%20b%2Fc"

echo "conditional writes"
etag=$(hdr "$URL/kv/a" | sed -n 's/^ETag: "\(.*\)"$/\1/p')
check "add over existing" 412 -X PUT -H 'If-None-Match: *' -d x "$URL/kv/a"
check "add new"           201 -X PUT -H 'If-None-Match: *' -d x "$URL/kv/new"
check "cas mismatch"      412 -X PUT -H 'If-Match: "999999"' -d x "$URL/kv/a"
check "cas match"         204 -X PUT -H "If-Match: \"$etag\"" -d cas "$URL/kv/a"
body  "cas applied"       cas "$URL/kv/a"
check "replace missing"   412 -X PUT -H 'If-Match: *' -d x "$URL/kv/absent"
check "cas delete wrong"  412 -X DELETE -H 'If-Match: "999999"' "$URL/kv/a"

echo "atomics"
curl -sS -o /dev/null -X DELETE "$URL/kv/n"
body  "incr creates"      1 -X POST "$URL/incr/n"
body  "incr by"           11 -X POST "$URL/incr/n?by=10"
body  "decr"              8 -X POST "$URL/decr/n?by=3"
body  "incr init"         42 -X POST "$URL/incr/m?by=2&init=40"
check "incr on text"      409 -X POST "$URL/incr/a"
check "append"            204 -X POST -d TAIL "$URL/append/a"
check "prepend"           204 -X POST -d HEAD "$URL/prepend/a"
body  "concatenated"      HEADcasTAIL "$URL/kv/a"
check "append to miss"    404 -X POST -d x "$URL/append/absent"

echo "ttl"
check "put with ttl"      201 -X PUT -d t "$URL/kv/t?ttl=1"
body  "ttl visible"       t "$URL/kv/t"
check "touch"             204 -X POST "$URL/touch/t?ttl=0"
sleep 1.2
body  "touch cleared ttl" t "$URL/kv/t"
check "short ttl"         204 -X PUT -d t "$URL/kv/t?ttlms=200"
sleep 0.5
check "expired"           404 "$URL/kv/t"
check "bad ttl"           400 -X PUT -d t "$URL/kv/t?ttl=abc"

echo "batch operations"
printf 'ba 2\nAA\nbb 2\nBB\n' > "$DIR/mset"
printf 'ba\nbb\nabsent\n' > "$DIR/mkeys"
check "mset"              204 -X POST --data-binary "@$DIR/mset" "$URL/mset"
body  "mset stored first" AA "$URL/kv/ba"
body  "mset stored last"  BB "$URL/kv/bb"
got=$(curl -sS -m 10 -X POST --data-binary "@$DIR/mkeys" "$URL/mget" | tr '\n' '|')
if [ "$got" = "2|AA|2|BB|-1|" ]; then
	ok "mget frames"
else
	bad "mget frames" "got '$got'"
fi
hdr -X POST --data-binary "@$DIR/mkeys" "$URL/mget" | grep -q '^X-Kache-Hits: 2$' \
	&& ok "mget hit count" || bad "mget hit count" "header missing"
check "mget empty body"   200 -X POST --data-binary '' "$URL/mget"
check "mset bad length"   400 -X POST --data-binary 'zz notanumber' "$URL/mset"
check "mset truncated"    400 -X POST --data-binary 'zz 99
short' "$URL/mset"
check "nothing written"   404 "$URL/kv/zz"
check "mdel"              204 -X POST --data-binary "@$DIR/mkeys" "$URL/mdel"
check "mdel removed"      404 "$URL/kv/ba"
check "mget on GET"       404 "$URL/mget"
# length prefixing is the whole reason the frames are not delimited
printf 'one\ntwo\n' > "$DIR/binary"
curl -sS -m 10 -X PUT --data-binary "@$DIR/binary" "$URL/kv/nl" -o /dev/null
got=$(curl -sS -m 10 -X POST --data-binary 'nl' "$URL/mget" | od -An -c | tr -s ' ')
case "$got" in
*'8 \n o n e \n t w o \n \n'*) ok "mget binary safe" ;;
*) bad "mget binary safe" "got '$got'" ;;
esac

echo "nested maps"
check "map put field"     201 -X PUT -d alice "$URL/kkv/u1?f=name"
body  "map get field"     alice "$URL/kkv/u1?f=name"
check "map second field"  201 -X PUT -d 30 "$URL/kkv/u1?f=age"
check "map replace field" 204 -X PUT -d 31 "$URL/kkv/u1?f=age"
body  "map replaced"      31 "$URL/kkv/u1?f=age"
body  "map incr field"    36 -X POST "$URL/kkvincr/u1?f=age&by=5"
body  "map decr field"    34 -X POST "$URL/kkvdecr/u1?f=age&by=2"
check "map missing field" 404 "$URL/kkv/u1?f=nope"
check "map missing key"   404 "$URL/kkv/nosuch?f=x"
check "map needs a field" 400 -X POST "$URL/kkvincr/u1"
h=$(hdr "$URL/kkv/u1?n=0")
has   "map field count"   "$h" '^X-Kache-Count: 2$'
got=$(curl -sS -m 10 "$URL/kkv/u1" | tr '\n' '|')
case "$got" in
*'4 5 -1|namealice|'*) ok "map dump frames" ;;
*) bad "map dump frames" "got '$got'" ;;
esac
check "map delete field"  204 -X DELETE "$URL/kkv/u1?f=age"
h=$(hdr "$URL/kkv/u1?n=0")
has   "map count after"   "$h" '^X-Kache-Count: 1$'
check "map drop"          204 -X DELETE "$URL/kkv/u1"
check "map gone"          404 "$URL/kkv/u1?f=name"
# an empty map stops existing, so the last field taking the key with it
check "map last field"    201 -X PUT -d only "$URL/kkv/u2?f=solo"
check "map remove last"   204 -X DELETE "$URL/kkv/u2?f=solo"
check "map went with it"  404 "$URL/kkv/u2"

echo "map conditional writes"
fetag=$(hdr -X PUT -d v1 "$URL/kkv/c1?f=x" | sed -n 's/^ETag: "\(.*\)"$/\1/p')
check "map cas mismatch"  412 -X PUT -H 'If-Match: "999999"' -d v2 "$URL/kkv/c1?f=x"
check "map cas match"     204 -X PUT -H "If-Match: \"$fetag\"" -d v2 "$URL/kkv/c1?f=x"
body  "map cas applied"   v2 "$URL/kkv/c1?f=x"
check "map add existing"  412 -X PUT -H 'If-None-Match: *' -d v3 "$URL/kkv/c1?f=x"
check "map add new"       201 -X PUT -H 'If-None-Match: *' -d v3 "$URL/kkv/c1?f=y"
check "map replace absent" 412 -X PUT -H 'If-Match: *' -d v "$URL/kkv/c1?f=z"

echo "map ttls"
check "map field ttl"     201 -X PUT -d short "$URL/kkv/t1?f=a&ttlms=200"
check "map field forever" 201 -X PUT -d long "$URL/kkv/t1?f=b"
sleep 0.5
check "map field expired" 404 "$URL/kkv/t1?f=a"
body  "map field kept"    long "$URL/kkv/t1?f=b"
check "map key ttl"       201 -X PUT -d v "$URL/kkv/t2?f=a&kttlms=200"
sleep 0.5
check "map key expired"   404 "$URL/kkv/t2?f=a"
# a write to one field must not restart the clock on the key
check "map key ttl set"   201 -X PUT -d v "$URL/kkv/t3?f=a&kttl=100"
curl -sS -o /dev/null -X PUT -d w "$URL/kkv/t3?f=b"
h=$(hdr "$URL/kkv/t3?n=0")
has   "map key ttl kept"  "$h" '^X-Kache-Key-TTL: 100$'
check "map touch field"   204 -X POST "$URL/kkvtouch/t3?f=a&ttl=50"
check "map touch key"     204 -X POST "$URL/kkvtouch/t3?ttl=0"
h=$(hdr "$URL/kkv/t3?n=0")
has   "map key made permanent"    "$h" '^X-Kache-Key-TTL: -1$'

echo "map batches"
printf '1 3 0\na123\n2 4 0\nbbWXYZ\n' > "$DIR/kkvset"
check "map batch set"     204 -X POST --data-binary "@$DIR/kkvset" "$URL/kkv/b1"
body  "map batch first"   123 "$URL/kkv/b1?f=a"
body  "map batch second"  WXYZ "$URL/kkv/b1?f=bb"
# a dump is a legal body for a write, which is what makes a copy one line
curl -sS -m 10 -o "$DIR/kkvdump" "$URL/kkv/b1"
check "map dump reloads"  204 -X POST --data-binary "@$DIR/kkvdump" "$URL/kkv/b2"
body  "map copy intact"   WXYZ "$URL/kkv/b2?f=bb"
printf '1\na\n2\nbb\n' > "$DIR/kkvdel"
check "map batch delete"  204 -X POST --data-binary "@$DIR/kkvdel" "$URL/kkvdel/b1"
check "map emptied"       404 "$URL/kkv/b1"
check "map batch junk"    400 -X POST --data-binary 'x 1
v' "$URL/kkv/b3"
check "map junk stored nothing" 404 "$URL/kkv/b3"
# lengths are explicit, so a field name may contain anything at all
printf '3 5 0\na\nb\000x\ny\n' > "$DIR/kkvbin"
check "map binary frame"  204 -X POST --data-binary "@$DIR/kkvbin" "$URL/kkv/b4"
curl -sS -m 10 -o "$DIR/kkvbinout" "$URL/kkv/b4"
got=$(od -An -c < "$DIR/kkvbinout" | tr -s ' ')
case "$got" in
*'a \n b \0 x \n y \n'*) ok "map binary safe" ;;
*) bad "map binary safe" "got '$got'" ;;
esac

echo "type safety"
curl -sS -o /dev/null -X PUT -d plain "$URL/kv/mixed"
check "map on a value"    409 "$URL/kkv/mixed?f=a"
check "queue on a value"  409 -X POST -d x "$URL/q/mixed"
curl -sS -o /dev/null -X PUT -d f "$URL/kkv/mixed2?f=a"
check "value on a map"    409 "$URL/kv/mixed2"
check "queue on a map"    409 -X POST -d x "$URL/q/mixed2"
check "delete crosses types" 204 -X DELETE "$URL/kv/mixed2"

echo "queues"
check "queue push"        201 -X POST -d job1 "$URL/q/w1"
check "queue push more"   204 -X POST -d job2 "$URL/q/w1"
curl -sS -o /dev/null -X POST -d job3 "$URL/q/w1"
h=$(hdr -I "$URL/q/w1")
has   "queue length"      "$h" '^X-Kache-Count: 3$'
body  "queue peek head"   job1 "$URL/q/w1"
body  "queue peek tail"   job3 "$URL/q/w1?side=r"
body  "queue pop is fifo" job1 -X POST "$URL/qpop/w1"
body  "queue pop again"   job2 -X POST "$URL/qpop/w1"
got=$(curl -sS -m 10 -X POST "$URL/qpop/w1?n=5" | tr '\n' '|')
case "$got" in
'4 -1 3|job3|') ok "queue framed pop" ;;
*) bad "queue framed pop" "got '$got'" ;;
esac
check "queue drained"     204 -X POST "$URL/qpop/w1"
check "queue push left"   201 -X POST -d b "$URL/q/w2"
curl -sS -o /dev/null -X POST -d a "$URL/q/w2?side=l"
body  "queue left end"    a "$URL/q/w2"
check "queue drop"        204 -X DELETE "$URL/q/w2"
check "queue gone"        204 -X POST "$URL/qpop/w2"

echo "queue batches and moves"
printf '4 0\njobA\n4 0\njobB\n4 0\njobC\n' > "$DIR/qpush"
check "queue batch push"  201 -X POST --data-binary "@$DIR/qpush" "$URL/qpush/w3"
h=$(hdr -I "$URL/q/w3")
has   "queue batch count" "$h" '^X-Kache-Count: 3$'
body  "queue move"        jobA -X POST "$URL/qmove/w3?dst=w4"
body  "queue moved to"    jobA "$URL/q/w4"
h=$(hdr -I "$URL/q/w3")
has   "queue move took"   "$h" '^X-Kache-Count: 2$'
check "queue move empty"  204 -X POST "$URL/qmove/nosuchq?dst=w4"
check "queue move needs dst" 400 -X POST "$URL/qmove/w3"
check "queue trim"        204 -X POST "$URL/qtrim/w3?maxlen=1"
body  "queue trim kept newest" jobC "$URL/q/w3"
for m in m1 m2 m3 m4; do curl -sS -o /dev/null -X POST -d "$m" "$URL/q/w5?maxlen=2"; done
got=$(curl -sS -m 10 "$URL/q/w5?n=9" | tr '\n' '|')
case "$got" in
*'m3|'*'m4|') ok "queue maxlen on push" ;;
*) bad "queue maxlen on push" "got '$got'" ;;
esac
check "queue entry ttl"   201 -X POST -d gone "$URL/q/w6?ttlms=200"
curl -sS -o /dev/null -X POST -d stays "$URL/q/w6"
sleep 0.5
body  "queue skipped expired" stays -X POST "$URL/qpop/w6"
check "queue touch"       201 -X POST -d v "$URL/q/w7"
check "queue key ttl"     204 -X POST "$URL/qtouch/w7?ttlms=200"
sleep 0.5
check "queue key expired" 204 -X POST "$URL/qpop/w7"
check "queue bad side"    400 -X POST "$URL/qpop/w3?side=middle"

echo "containers at scale"
awk 'BEGIN { for (i = 0; i < 2000; i++) printf "%d %d 0\nf%dvalue-%d\n", length("f" i), length("value-" i), i, i }' \
	> "$DIR/bigmap"
check "big map set"       204 -X POST --data-binary "@$DIR/bigmap" "$URL/kkv/big"
h=$(hdr "$URL/kkv/big?n=0")
has   "big map count"     "$h" '^X-Kache-Count: 2000$'
body  "big map spot"      value-1999 "$URL/kkv/big?f=f1999"
got=$(curl -sS -m 10 "$URL/kkv/big?n=2000" | wc -l)
[ "$got" = 4000 ] && ok "big map dump" || bad "big map dump" "got $got lines"
awk 'BEGIN { for (i = 0; i < 2000; i++) printf "%d 0\nmsg-%d\n", length("msg-" i), i }' \
	> "$DIR/bigq"
check "big queue push"    201 -X POST --data-binary "@$DIR/bigq" "$URL/qpush/bigq"
body  "big queue head"    msg-0 "$URL/q/bigq"
body  "big queue tail"    msg-1999 "$URL/q/bigq?side=r"
got=$(curl -sS -m 10 -X POST "$URL/qpop/bigq?n=2000" | wc -l)
[ "$got" = 4000 ] && ok "big queue drain" || bad "big queue drain" "got $got lines"
check "big queue emptied" 204 -X POST "$URL/qpop/bigq"
# dropping a container is constant time; the sweeper does the work after
check "big map drop"      204 -X DELETE "$URL/kkv/big"
# nothing is asking after that key, so this waits on the housekeeping
# tick rather than on traffic
i=0
while [ "$(curl -sS -m 10 "$URL/metrics" | sed -n 's/^kache_reclaim_pending //p')" != 0 ]; do
	i=$((i + 1))
	[ $i -gt 40 ] && break
	sleep 0.25
done
pending=$(curl -sS -m 10 "$URL/metrics" | sed -n 's/^kache_reclaim_pending //p')
[ "$pending" = 0 ] && ok "deferred reclaim finishes" \
	|| bad "deferred reclaim finishes" "still $pending pending"
# A container lives in one shard, so it stops growing at that shard's
# arena.  What matters is that it stops cleanly and stays readable.
i=0
code=204
while [ "$code" = 204 ] && [ $i -lt 60 ]; do
	i=$((i + 1))
	awk -v r="$i" 'BEGIN {
		v = sprintf("%1000s", ""); gsub(/ /, "x", v)
		for (j = 0; j < 200; j++) {
			f = "b" r "_" j
			printf "%d %d 0\n%s%s\n", length(f), length(v), f, v
		}
	}' > "$DIR/fillmap"
	code=$(curl -sS -m 30 -o /dev/null -w '%{http_code}' -X POST \
		--data-binary "@$DIR/fillmap" "$URL/kkv/fullmap")
done
[ "$code" = 507 ] && ok "container stops at its shard" \
	|| bad "container stops at its shard" "got $code after $i batches"
count=$(hdr "$URL/kkv/fullmap?n=0" | sed -n 's/^X-Kache-Count: //p')
[ "${count:-0}" -gt 200 ] && ok "full container still readable" \
	|| bad "full container still readable" "count is ${count:-none}"
got=$(curl -sS -m 10 "$URL/kkv/fullmap?f=b1_0" | wc -c)
[ "$got" = 1000 ] && ok "full container spot read" \
	|| bad "full container spot read" "got $got bytes"
check "full container drops"  204 -X DELETE "$URL/kkv/fullmap"

echo "limits and errors"
long=$(printf 'k%.0s' $(seq 1 600))
check "key too long"      414 -X PUT -d x "$URL/kv/$long"
dd if=/dev/zero of="$DIR/big" bs=1024 count=300 2>/dev/null
check "value too large"   413 -X PUT --data-binary "@$DIR/big" "$URL/kv/big"
check "bad method"        405 -X OPTIONS "$URL/kv/a"
printf 'BOGUS\r\n\r\n' | timeout 5 nc -q1 127.0.0.1 "$PORT" | head -1 | grep -q '400' \
	&& ok "malformed request" || bad "malformed request" "no 400"

echo "introspection"
check "stats"             200 "$URL/stats"
check "metrics"           200 "$URL/metrics"
curl -sS "$URL/metrics" | grep -q '^kache_keys ' && ok "metric names" \
	|| bad "metric names" "kache_keys missing"

echo "flush and persistence"
check "flush"             204 -X POST "$URL/flush"
check "flushed"           404 "$URL/kv/a"
check "reput"             201 -X PUT -d survivor "$URL/kv/keep"
curl -sS -o /dev/null -X PUT -d mapvalue "$URL/kkv/keepmap?f=fld"
curl -sS -o /dev/null -X POST -d queued "$URL/q/keepq"
kill -TERM "$pid"; wait "$pid" 2>/dev/null; pid=
"$BIN" -f "$DIR/test.db" -s 64M -p "$PORT" -F -q >>"$DIR/log" 2>&1 &
pid=$!
i=0
while ! curl -sS -m 1 -o /dev/null "$URL/health" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "restart failed"; cat "$DIR/log"; exit 1; }
	sleep 0.1
done
body  "survived restart"  survivor "$URL/kv/keep"
body  "map survived restart"   mapvalue "$URL/kkv/keepmap?f=fld"
body  "queue survived restart" queued "$URL/q/keepq"
kill -KILL "$pid"; wait "$pid" 2>/dev/null; pid=
"$BIN" -f "$DIR/test.db" -s 64M -p "$PORT" -F -q >>"$DIR/log" 2>&1 &
pid=$!
i=0
while ! curl -sS -m 1 -o /dev/null "$URL/health" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "recovery failed"; cat "$DIR/log"; exit 1; }
	sleep 0.1
done
body  "survived a crash"  survivor "$URL/kv/keep"
body  "map survived a crash"   mapvalue "$URL/kkv/keepmap?f=fld"
body  "queue survived a crash" queued "$URL/q/keepq"

echo "key decoding"
check "escaped key put"   201 -X PUT -d roundtrip "$URL/kv/abc"
body  "escaped key get"   roundtrip "$URL/kv/%61%62c"
kmax=$(printf 'k%.0s' $(seq 1 255))
check "key at the limit"  201 -X PUT -d atmax "$URL/kv/$kmax"
body  "limit key readback" atmax "$URL/kv/$kmax"
check "key over the limit" 414 -X PUT -d over "$URL/kv/${kmax}k"
# 765 raw bytes of escapes decoding to the same 255 byte key: the case that
# breaks if the length test is hoisted out of the unescaped branch
kesc=$(printf '%%6b%.0s' $(seq 1 255))
body  "all escape key"    atmax "$URL/kv/$kesc"

echo "minimal responses"
PORT2=${PORT2:-$((PORT + 1))}
URL2="http://127.0.0.1:$PORT2"
"$BIN" -f "$DIR/min.db" -s 64M -p "$PORT2" -F -q -M >>"$DIR/log" 2>&1 &
pid2=$!
i=0
while ! curl -sS -m 1 -o /dev/null "$URL2/health" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "minimal server did not start"; cat "$DIR/log"; exit 1; }
	sleep 0.1
done

check "minimal put"       201 -X PUT -d minimal "$URL2/kv/m"
h=$(hdr "$URL2/kv/m")
lacks "minimal no server"     "$h" '^Server:'
lacks "minimal no type"       "$h" '^Content-Type:'
lacks "minimal no etag"       "$h" '^ETag:'
lacks "minimal no ttl"        "$h" '^X-Kache-TTL:'
lacks "minimal no flags"      "$h" '^X-Kache-Flags:'
lacks "minimal no connection" "$h" '^Connection:'
has   "minimal keeps date"    "$h" '^Date: ...,'
has   "minimal length"        "$h" '^Content-Length: 7$'
printf 'one\ntwo\n' > "$DIR/mbody"
curl -sS -m 10 -o /dev/null -X PUT --data-binary "@$DIR/mbody" "$URL2/kv/mb"
curl -sS -m 10 -o "$DIR/mgot" "$URL2/kv/mb"
cmp -s "$DIR/mbody" "$DIR/mgot" && ok "minimal body intact" \
	|| bad "minimal body intact" "value came back changed"
has   "minimal binary length" "$(hdr "$URL2/kv/mb")" '^Content-Length: 8$'

# the write side keeps every header, so cas and conditionals still work
etag2=$(hdr -X PUT -d v1 "$URL2/kv/c" | sed -n 's/^ETag: "\(.*\)"$/\1/p')
[ -n "$etag2" ] && ok "minimal put etag" || bad "minimal put etag" "no ETag"
check "minimal cas match"    204 -X PUT -H "If-Match: \"$etag2\"" -d v2 "$URL2/kv/c"
check "minimal cas stale"    412 -X PUT -H "If-Match: \"$etag2\"" -d v3 "$URL2/kv/c"
body  "minimal cas applied"  v2 "$URL2/kv/c"
has   "minimal closes"       "$(hdr -H 'Connection: close' "$URL2/kv/m")" \
	'^Connection: close$'

# reply_text is not what minimal mode trims, so the type survives, but
# hdrs_end still leaves the keep-alive line unsaid
h=$(hdr "$URL2/kv/absent")
has   "minimal 404 type"       "$h" '^Content-Type: text/plain; charset=utf-8$'
lacks "minimal 404 connection" "$h" '^Connection:'

# deliberate asymmetry: the framed body cannot be read without these
printf 'm\nabsent\n' > "$DIR/mkeys2"
h=$(hdr -X POST --data-binary "@$DIR/mkeys2" "$URL2/mget")
has   "minimal mget type"  "$h" '^Content-Type: application/octet-stream$'
has   "minimal mget count" "$h" '^X-Kache-Count: 2$'

# and the default server still says all of it
check "default put"        201 -X PUT -d normal "$URL/kv/full"
h=$(hdr "$URL/kv/full")
has   "default server"     "$h" '^Server: kache$'
has   "default type"       "$h" '^Content-Type: application/octet-stream$'
has   "default etag"       "$h" '^ETag: "'
has   "default ttl"        "$h" '^X-Kache-TTL:'
has   "default flags"      "$h" '^X-Kache-Flags:'
has   "default keep-alive" "$h" '^Connection: keep-alive$'

echo "hot set"
# A third server with the per worker hot set on and one worker, so every
# request lands on the same set and the admission threshold is reached.
# One worker is what makes the invalidation assertions deterministic: a
# write only retires the set of the worker that took it.
PORT3=${PORT3:-$((PORT + 2))}
URL3="http://127.0.0.1:$PORT3"
"$BIN" -f "$DIR/hot.db" -s 64M -p "$PORT3" -F -q -t 1 -X 60000 >>"$DIR/log" 2>&1 &
pid3=$!
i=0
while ! curl -sS -m 1 -o /dev/null "$URL3/health" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "hot server did not start"; cat "$DIR/log"; exit 1; }
	sleep 0.1
done

# warm returns once the key has been asked for enough times to be
# admitted; CFG_HOT_ADMIT is 32, so 40 is comfortably past it
warm() {
	i=0
	while [ $i -lt 40 ]; do
		curl -sS -m 10 -o /dev/null "$1" 2>/dev/null
		i=$((i + 1))
	done
}

curl -sS -m 10 -o /dev/null -X PUT -d hv1 "$URL3/kv/hk"
warm "$URL3/kv/hk"
body  "hot serves the value"   hv1 "$URL3/kv/hk"
has   "hot set is being used"  "$(curl -sS "$URL3/stats")" '^hot_hits_total [1-9]'

# a write on this worker must retire the set, or the old value lingers
curl -sS -m 10 -o /dev/null -X PUT -d hv2 "$URL3/kv/hk"
body  "write retires the set"  hv2 "$URL3/kv/hk"
curl -sS -m 10 -o /dev/null -X POST -d x "$URL3/append/hk"
body  "append retires it"      hv2x "$URL3/kv/hk"
curl -sS -m 10 -o /dev/null -X DELETE "$URL3/kv/hk"
check "delete retires it"      404 "$URL3/kv/hk"

# two keys that are both hot must not be served each other's bytes
curl -sS -m 10 -o /dev/null -X PUT -d aaa "$URL3/kv/ha"
curl -sS -m 10 -o /dev/null -X PUT -d bbb "$URL3/kv/hb"
warm "$URL3/kv/ha"
warm "$URL3/kv/hb"
body  "hot key a is itself"    aaa "$URL3/kv/ha"
body  "hot key b is itself"    bbb "$URL3/kv/hb"

# a replayed response carries a fresh Date, not the one it was built with
curl -sS -m 10 -o /dev/null -X PUT -d dv "$URL3/kv/hd"
warm "$URL3/kv/hd"
d1=$(hdr "$URL3/kv/hd" | sed -n 's/^Date: //p')
sleep 2
d2=$(hdr "$URL3/kv/hd" | sed -n 's/^Date: //p')
[ -n "$d1" ] && [ "$d1" != "$d2" ] && ok "replayed date is refreshed" \
	|| bad "replayed date is refreshed" "date stuck at '$d1'"
has   "replayed response intact" "$(hdr "$URL3/kv/hd")" '^Content-Length: 2$'

# a body with a NUL must survive being replayed out of the set
printf 'a\0b' > "$DIR/hbin"
curl -sS -m 10 -o /dev/null -X PUT --data-binary "@$DIR/hbin" "$URL3/kv/hbin"
warm "$URL3/kv/hbin"
curl -sS -m 10 -o "$DIR/hgot" "$URL3/kv/hbin"
cmp -s "$DIR/hbin" "$DIR/hgot" && ok "hot binary body intact" \
	|| bad "hot binary body intact" "value came back changed"

# A conditional GET is not answered from the set.  kache does not act on
# If-None-Match when reading - a conditional GET is answered in full, the
# same as any other - so what this pins is that the hot set does not
# change that answer, and that the exclusion in hot_ok stays in place for
# whenever reading does learn to answer 304.
etag3=$(hdr "$URL3/kv/ha" | sed -n 's/^ETag: "\(.*\)"$/\1/p')
[ -n "$etag3" ] && ok "hot response carries an etag" \
	|| bad "hot response carries an etag" "no ETag on a replayed response"
check "conditional get is not cached" 200 -H "If-None-Match: \"$etag3\"" "$URL3/kv/ha"
body  "conditional get is correct"    aaa -H "If-None-Match: \"$etag3\"" "$URL3/kv/ha"

kill "$pid3" 2>/dev/null; wait "$pid3" 2>/dev/null; pid3=


echo "cluster"
# Two nodes, each a full copy.  Ports are separate from the servers above
# because those hold their own stores and this pair needs a shared hash
# seed, which -C derives from the member list.
PORT4=${PORT4:-$((PORT + 3))}
PORT5=${PORT5:-$((PORT + 4))}
CNODES="127.0.0.1:$PORT4,127.0.0.1:$PORT5"
U4="http://127.0.0.1:$PORT4"
U5="http://127.0.0.1:$PORT5"
"$BIN" -f "$DIR/c0.db" -s 64M -p "$PORT4" -n -q -C "$CNODES" -N 0 -Y 20 >>"$DIR/log" 2>&1 &
pid4=$!
"$BIN" -f "$DIR/c1.db" -s 64M -p "$PORT5" -n -q -C "$CNODES" -N 1 -Y 20 >>"$DIR/log" 2>&1 &
pid5=$!
i=0
while ! curl -sS -m 1 -o /dev/null "$U4/health" 2>/dev/null ||
      ! curl -sS -m 1 -o /dev/null "$U5/health" 2>/dev/null; do
	i=$((i + 1))
	[ $i -gt 50 ] && { echo "cluster did not start"; cat "$DIR/log"; exit 1; }
	sleep 0.1
done

has "cluster size is known" "$(curl -sS "$U4/stats")" '^cluster_nodes 2$'

# Both nodes must name the same owner for a key.  One of them answers the
# write itself and the other redirects to it; which is which depends on
# the hash, so the test asserts they agree rather than naming a node.
o4=$(curl -sS -o /dev/null -w '%{redirect_url}' -m 10 -X PUT -d x "$U4/kv/ck")
o5=$(curl -sS -o /dev/null -w '%{redirect_url}' -m 10 -X PUT -d x "$U5/kv/ck")
if [ -n "$o4" ] && [ -z "$o5" ]; then
	ok "nodes agree on the owner"
	owner=$U5
elif [ -z "$o4" ] && [ -n "$o5" ]; then
	ok "nodes agree on the owner"
	owner=$U4
else
	bad "nodes agree on the owner" "both claimed or both redirected"
	owner=$U4
fi
case "${o4}${o5}" in
	*"/kv/ck"*) ok  "redirect keeps the path" ;;
	*)          bad "redirect keeps the path" "got '${o4}${o5}'" ;;
esac

# A write to the owner reaches the other node within a flush interval.
curl -sS -m 10 -o /dev/null -X PUT -d replicated "$owner/kv/ck"
sleep 0.3
body "replicated to node 0" replicated "$U4/kv/ck"
body "replicated to node 1" replicated "$U5/kv/ck"

# The last write wins everywhere, and coalescing means far fewer frames
# than writes were shipped to get there.
# The burst goes down one connection in a single curl, because that is the
# condition coalescing exists for: writes arriving faster than the flush
# interval.  One curl per write would spend longer starting the process
# than the window lasts, and every write would get a window to itself -
# which is correct behaviour and would tell us nothing.
sent0=$(curl -sS "$owner/stats" | awk '/^repl_sent_total/{print $2}')
set -- -o /dev/null -X PUT -d w0 "$owner/kv/ck"
i=1
while [ $i -lt 60 ]; do
	set -- "$@" --next -o /dev/null -X PUT -d "w$i" "$owner/kv/ck"
	i=$((i + 1))
done
curl -sS -m 30 "$@" >/dev/null 2>&1
sleep 0.3
body "converged on node 0" w59 "$U4/kv/ck"
body "converged on node 1" w59 "$U5/kv/ck"
sent1=$(curl -sS "$owner/stats" | awk '/^repl_sent_total/{print $2}')
[ "$((sent1 - sent0))" -lt 60 ] && ok "writes were coalesced" \
	|| bad "writes were coalesced" "60 writes shipped $((sent1 - sent0)) frames"

# A delete has to travel as a tombstone; without one the peer would keep
# serving the copy it already had.
curl -sS -m 10 -o /dev/null -X DELETE "$owner/kv/ck"
sleep 0.3
check "delete reached node 0" 404 "$U4/kv/ck"
check "delete reached node 1" 404 "$U5/kv/ck"

# A store created under a different member list hashes differently, so
# joining with it would split the keyspace silently.  It must not start.
if "$BIN" -f "$DIR/c0.db" -s 64M -p "$PORT4" -q -C "127.0.0.1:9,127.0.0.1:8" \
     -N 0 >>"$DIR/log" 2>&1; then
	bad "mismatched seed is refused" "started anyway"
else
	ok "mismatched seed is refused"
fi

kill "$pid4" "$pid5" 2>/dev/null; wait "$pid4" 2>/dev/null; wait "$pid5" 2>/dev/null
pid4=; pid5=


echo
if [ "$fails" -eq 0 ]; then
	echo "all checks passed"
else
	echo "$fails check(s) failed"
fi
exit $((fails > 0))
