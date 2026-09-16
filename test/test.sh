#!/bin/sh
# kache - end to end checks.  Run with `make check`.
set -u

PORT=${PORT:-17071}
DIR=$(mktemp -d)
BIN=./kache
URL="http://127.0.0.1:$PORT"
fails=0
pid=

cleanup() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
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

echo
if [ "$fails" -eq 0 ]; then
	echo "all checks passed"
else
	echo "$fails check(s) failed"
fi
exit $((fails > 0))
