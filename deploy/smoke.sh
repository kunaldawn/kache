#!/bin/sh
# kache - prove the compose cluster is a cluster, not three servers.
#
#     docker compose up --build -d && sh deploy/smoke.sh
#
# Everything here goes through the published ports, the way a client on
# the host would, so it exercises kache-lb and the redirects as well as
# the cluster itself.
set -u

LB=${LB:-http://127.0.0.1:7080}
N0=${N0:-http://127.0.0.1:7071}
N1=${N1:-http://127.0.0.1:7072}
N2=${N2:-http://127.0.0.1:7073}
fails=0

ok()  { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s: %s\n' "$1" "$2"; fails=$((fails + 1)); }
chk() { [ "$2" = "$3" ] && ok "$1" || bad "$1" "expected '$3', got '$2'"; }

# all NODES prints one node's answer for a key, space separated
all() { for u in "$N0" "$N1" "$N2"; do
		printf '%s ' "$(curl -sS -m 5 "$u/kv/$1" 2>/dev/null)"
	done; }

echo "waiting for the cluster"
i=0
while :; do
	up=0
	for u in "$N0" "$N1" "$N2" "$LB"; do
		curl -sS -m 2 -o /dev/null "$u/health" 2>/dev/null && up=$((up + 1))
	done
	[ "$up" -eq 4 ] && break
	i=$((i + 1))
	if [ "$i" -gt 60 ]; then
		echo "cluster did not come up; try: docker compose logs"
		exit 1
	fi
	sleep 1
done
ok "all three nodes and the balancer are answering"

echo
echo "membership"
chk "every node sees three peers" \
    "$(curl -sS "$N0/stats" | awk '/^cluster_nodes/{print $2}')" "3"
ids=$(for u in "$N0" "$N1" "$N2"; do
	curl -sS "$u/stats" | awk '/^cluster_self/{printf "%s", $2}'
      done)
chk "each node knows its own index" "$ids" "012"

echo
echo "ownership - every node must name the same owner for a key"
for k in alpha beta gamma delta; do
	seen=""
	for u in "$N0" "$N1" "$N2"; do
		loc=$(curl -sS -m 5 -o /dev/null -w '%{redirect_url}' \
		      -X PUT -d probe "$u/kv/$k" 2>/dev/null)
		# empty redirect_url means this node answered it, so it is
		# the owner; otherwise the owner is whoever it pointed at
		if [ -z "$loc" ]; then
			seen="$seen $u"
		else
			seen="$seen $(printf '%s' "$loc" | sed 's#/kv/.*##')"
		fi
	done
	uniq=$(printf '%s' "$seen" | tr ' ' '\n' | sort -u | grep -c .)
	if [ "$uniq" -eq 1 ]; then
		ok "$k -> $(printf '%s' "$seen" | awk '{print $1}')"
	else
		bad "$k ownership" "nodes disagreed:$seen"
	fi
done

echo
echo "replication"
curl -sSL -m 5 -X PUT -d hello-cluster "$LB/kv/greeting" >/dev/null 2>&1
sleep 0.5
chk "a write through the balancer reached every node" \
    "$(all greeting)" "hello-cluster hello-cluster hello-cluster "

# Written straight at a node that probably does not own it, so this is
# the redirect rewriting being exercised end to end.
curl -sSL -m 5 -X PUT -d second "$N2/kv/greeting" >/dev/null 2>&1
sleep 0.5
chk "a redirected write reached every node" \
    "$(all greeting)" "second second second "

curl -sSL -m 5 -X DELETE "$LB/kv/greeting" >/dev/null 2>&1
sleep 0.5
codes=""
for u in "$N0" "$N1" "$N2"; do
	codes="$codes$(curl -sS -m 5 -o /dev/null -w '%{http_code} ' "$u/kv/greeting")"
done
chk "a delete reached every node" "$codes" "404 404 404 "

echo
echo "reads are answered locally, by whichever node they land on"
curl -sSL -m 5 -X PUT -d spread "$LB/kv/rr" >/dev/null 2>&1
sleep 0.5
before=$(for u in "$N0" "$N1" "$N2"; do
		curl -sS "$u/stats" | awk '/^hits_total/{printf "%s ", $2}'
	 done)
i=0
while [ $i -lt 60 ]; do
	curl -sS -m 5 -o /dev/null "$LB/kv/rr"
	i=$((i + 1))
done
after=$(for u in "$N0" "$N1" "$N2"; do
		curl -sS "$u/stats" | awk '/^hits_total/{printf "%s ", $2}'
	 done)
served=0
set -- $before
for a in $after; do
	b=$1; shift
	[ "$((a - b))" -gt 0 ] && served=$((served + 1))
done
chk "all three nodes served some of the reads" "$served" "3"

echo
echo "fanout coalescing - the property that makes a write hot key work"
# Find the owner so the writes are not all redirects, then burst them
# down one connection: writes arriving faster than -Y is the whole case.
owner=$N0
for u in "$N0" "$N1" "$N2"; do
	[ -z "$(curl -sS -m 5 -o /dev/null -w '%{redirect_url}' \
	        -X PUT -d probe "$u/kv/hotkey")" ] && owner=$u
done
s0=$(curl -sS "$owner/stats" | awk '/^repl_sent_total/{print $2}')
w0=$(curl -sS "$owner/stats" | awk '/^sets_total/{print $2}')
set -- -o /dev/null -X PUT -d w0 "$owner/kv/hotkey"
i=1
while [ $i -lt 200 ]; do
	set -- "$@" --next -o /dev/null -X PUT -d "w$i" "$owner/kv/hotkey"
	i=$((i + 1))
done
curl -sS -m 60 "$@" >/dev/null 2>&1
sleep 0.5
s1=$(curl -sS "$owner/stats" | awk '/^repl_sent_total/{print $2}')
w1=$(curl -sS "$owner/stats" | awk '/^sets_total/{print $2}')
printf '    %s writes -> %s frames shipped to peers\n' \
    "$((w1 - w0))" "$((s1 - s0))"
[ "$((s1 - s0))" -lt "$((w1 - w0))" ] && ok "writes were coalesced" \
	|| bad "writes were coalesced" "shipped as many frames as writes"
chk "every node converged on the last value" "$(all hotkey)" "w199 w199 w199 "

echo
echo "hot set (-X 50) - answered without reaching the store"
# This has to be a burst down one connection, for two reasons.  A key is
# admitted only after CFG_HOT_ADMIT sightings inside one decay window, and
# the set is per worker - so requests dribbled out one curl at a time, over
# several seconds and spread across every worker, are exactly what the
# doorkeeper is built to refuse.  That is the feature working, not a
# failure, but it demonstrates nothing.  Several hundred requests on one
# connection is what a hot key actually looks like.
set -- -o /dev/null "$N0/kv/hotkey"
i=1
while [ $i -lt 600 ]; do
	set -- "$@" --next -o /dev/null "$N0/kv/hotkey"
	i=$((i + 1))
done
curl -sS -m 60 "$@" >/dev/null 2>&1
hh=$(curl -sS "$N0/stats" | awk '/^hot_hits_total/{print $2}')
[ "${hh:-0}" -gt 0 ] && ok "node 0 served $hh requests from its hot set" \
	|| bad "hot set is in use" "hot_hits_total is $hh"

echo
if [ "$fails" -eq 0 ]; then
	echo "cluster is healthy"
	echo
	echo "  reads   curl $LB/kv/<key>"
	echo "  writes  curl -L -X PUT -d value $LB/kv/<key>   (-L follows"
	echo "          the 307 to whichever node owns the key)"
	echo "  bench   ./kache-bench -h 127.0.0.1 -p 7071 -W get -k 1 -P 16"
	echo "          7080 is kache-lb and is fine to benchmark too, but"
	echo "          give it concurrency: a proxy adds a hop, and at a"
	echo "          fixed depth throughput is concurrency over latency."
	echo "          -P 16 -t 2 measures 0.55x a node; -P 64 -t 8 is 1.28x"
else
	echo "$fails check(s) failed - docker compose logs will say why"
fi
exit $((fails > 0))
