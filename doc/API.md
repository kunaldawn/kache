kache HTTP interface
====================

HTTP/1.1 with keep alive and pipelining.  `Transfer-Encoding: chunked` is
not accepted (`501`); send a `Content-Length`.  `Expect: 100-continue` is
honoured.

Keys
----

The key is everything after the endpoint prefix, percent decoded, so
`/kv/a%20b`, `/kv/users/42` and `/kv/%00%01` are all valid and `/` needs
no escaping.  Keys are bytes, not text; the limit is `-K` (255 by
default) after decoding.  Values are bytes too, up to `-V`.

Common parameters
-----------------

| Parameter | Meaning |
| --- | --- |
| `?ttl=<seconds>` | expire after this long; `0` means never |
| `?ttlms=<ms>` | the same, in milliseconds; takes precedence |
| `?flags=<u32>` | opaque, stored and returned verbatim |

`X-Kache-TTL` (seconds) and `X-Kache-Flags` work as headers instead, and
the query string wins if both are present.  When a request carries no TTL
at all, the server default from `-e` applies.

Common response headers
-----------------------

| Header | Meaning |
| --- | --- |
| `ETag: "<n>"` | the CAS token; changes on every mutation |
| `X-Kache-TTL` | seconds left, `-1` when the entry never expires |
| `X-Kache-Flags` | the flags stored with the value |

A server started with `-M` leaves all three off a successful `GET`; see
Minimal responses.

A response to `HEAD` never carries a body, on any endpoint.  Its
`Content-Length` still describes the body a `GET` would have returned,
which is what makes `HEAD` useful for asking a key's size without
fetching it.

Endpoints
---------

### `GET /kv/<key>` and `HEAD /kv/<key>`

`200` with the value as `application/octet-stream`, or `404`.  `HEAD`
sends the same headers, `Content-Length` included, and no body.  A `GET`
here is the one response `-M` takes metadata away from; a `HEAD` keeps
it, having no body to save the bytes on.

### `PUT /kv/<key>`, `POST /kv/<key>`

Stores the request body.  `201` when the key was created, `204` when it
replaced a value.  Conditional forms:

| Request header | Meaning | On failure |
| --- | --- | --- |
| `If-None-Match: *` | store only if the key is absent | `412` |
| `If-Match: *` | store only if the key is present | `412` |
| `If-Match: "<etag>"` | store only if the value is unchanged | `412` |

A compare and swap is therefore a `GET` followed by a `PUT` carrying the
`ETag` you read, and `412` means someone got there first.  Under `-M` the
read has no `ETag` to give you, so keep the one the write that produced
the version already returned.

### `DELETE /kv/<key>`

`204`, or `404` if the key is not there.  `If-Match: "<etag>"` deletes
only that version and answers `412` otherwise.

### `POST /incr/<key>`, `POST /decr/<key>`

Reads the value as a decimal integer, adds (or subtracts) `?by=` which
defaults to `1`, stores the result and returns it as `text/plain`.
`?init=` supplies the value to start from when the key is absent, default
`0`, so incrementing a missing counter creates it.  `409` if the stored
value is not an integer or the result would overflow 64 bits.  The read,
the addition and the write happen under one lock.

### `POST /append/<key>`, `POST /prepend/<key>`

Concatenates the body onto the stored value and answers `204` with the
new `ETag` and `X-Kache-Length`.  `404` if the key is absent, `413` if the
result would exceed the value limit.  The TTL and flags are preserved.

### `POST /touch/<key>`

Applies a new TTL without touching the value.  `204`, or `404`.
`?ttl=0` makes the entry permanent.

Batch endpoints
---------------

One request, many keys.  A single `GET` spends about 230 ns in the store
and 27 microseconds getting there and back, so for a caller that needs
fifty keys the round trip is the whole cost.  Pipelining removes it too,
but almost no HTTP client library will pipeline, whereas any of them can
post a list.  Measured on loopback, against single `GET`s from the same
non-pipelining client: 5.1x at 8 keys a batch, 21x at 64, 33x at 256.

**A batch is not a snapshot.**  Each key is taken under its own shard
lock in turn, so a batch is N independent operations that happened to
share a request, and another client's write can land in the middle of
one.  Redis can promise otherwise because it runs commands on a single
thread.  Callers that already cope with a write landing between two
separate `GET`s need no changes; callers that need a real snapshot do not
want a cache.

A batch may carry up to 1024 keys (`CFG_BATCH_MAX`), and the body is
bounded like any other at `CFG_MAX_VAL + CFG_REQ_SLACK`.

### `POST /mget`

Body: one percent encoded key per line, exactly as they are written in a
path.  Blank lines are ignored.

Response: one frame per key, in order.  A hit is the value's length in
decimal, a newline, that many raw bytes, and a newline.  A miss is `-1`
and a newline.  Length prefixing is what keeps values binary safe.

    POST /mget                    200 OK
    alpha                         X-Kache-Count: 3
    beta                          X-Kache-Hits: 2
    missing
                                  5
                                  hello
                                  3
                                  abc
                                  -1

Per key metadata is deliberately absent: use `GET /kv/<key>` when you
need the `ETag` or the remaining TTL.

### `POST /mset`

Body: one record per line pair.  A header line of `<key> <bytes>` with an
optional third field giving that record's TTL in seconds, then exactly
that many raw bytes, then a newline.  The key is percent encoded; the
value is length prefixed and may contain anything.

    POST /mset?ttl=60
    alpha 5
    hello
    beta 3 300
    abc

`204` when every record was stored, with `X-Kache-Count` and
`X-Kache-Stored`.  `507` with the same headers when the arena refused
some of them, so a partial write is always visible rather than silent.

The whole body is parsed before anything is written, so a malformed
batch is rejected with `400` and changes nothing.  A batch cannot be
applied atomically, but it can be rejected atomically.

### `POST /mdel`

Body: one percent encoded key per line, as for `/mget`.  `204` with
`X-Kache-Count` and `X-Kache-Deleted`.

Nested maps
-----------

A key whose value is itself a key/value store.  The outer key carries a
TTL and so does every field, independently: a session map can hold
entries that lapse in a minute inside a key that lapses in a day.

The path names the outer key and the query names everything inside it,
because a second path segment cannot say where an outer key containing a
slash ends and the field begins - and keys here are bytes, not words.

| Parameter | Meaning |
| --- | --- |
| `?f=<field>` | the field, percent encoded exactly like a key |
| `?ttl=`, `?ttlms=` | the field's expiry |
| `?kttl=`, `?kttlms=` | the map's own expiry |
| `?n=<count>` | how many fields an enumeration may return |

`?kttl=` is applied only when the request carries it, so writing one
field of a long lived map does not quietly restart the clock on the map.

Every call on one map takes one lock - the shard its outer key hashes
to - so a read, a write, a whole map enumeration and a many field write
are each atomic against every other operation on that key.  This is what
a flattened `outer\0inner` key space cannot give you: there the pieces
land in different shards and nothing can be done to all of them at once.

**A map that loses its last field stops existing**, as an empty one has
nothing left to describe.  So does one whose own TTL runs out, live
fields and all.

### `GET /kkv/<key>?f=<field>`

The field's value, `200`, as `application/octet-stream`.  `ETag` is the
field's CAS token and `X-Kache-TTL` its remaining seconds, both of the
field rather than the map.  `404` if either the map or the field is
absent, `409` if the key holds something that is not a map.

### `PUT /kkv/<key>?f=<field>`, `POST /kkv/<key>?f=<field>`

Stores the body as that field, creating the map if it is not there.
`201` when the field was created, `204` when it replaced one.
`If-None-Match: *` and `If-Match:` work exactly as on `/kv/<key>`, and
apply to the field.

Response headers carry both levels: `ETag`, `X-Kache-TTL` and
`X-Kache-Flags` describe the field; `X-Kache-Count`, `X-Kache-Bytes`,
`X-Kache-Key-TTL` and `X-Kache-Key-Version` describe the map.

### `DELETE /kkv/<key>?f=<field>`

Removes the field.  `204`, or `404`.  `If-Match: "<etag>"` removes only
that version.

### `DELETE /kkv/<key>`

Removes the whole map, however large, in constant time - see *Deferred
reclamation* below.  `204`, or `404`.

### `GET /kkv/<key>`, `HEAD /kkv/<key>`

Every live field, framed:

    "<fieldlen> <valuelen> <ttl>\n" <field bytes> <value bytes> "\n"

Both lengths are explicit, so field names and values are binary safe
alike, and `<ttl>` is seconds or `-1`.  A dump is itself a legal body
for `POST /kkv/<key>`, so copying a map is two requests.

    GET /kkv/user:1               200 OK
                                  X-Kache-Count: 2
                                  X-Kache-Returned: 2
                                  4 5 -1
                                  namealice
                                  3 2 60
                                  age30

At most `?n=` fields come back, capped at `CFG_CONT_BATCH_MAX` (4096) and
at 8 MiB of body, and `X-Kache-Truncated: 1` says when that bit.  `?n=0`
asks for the counters alone and costs one lookup instead of a walk.
There is no cursor: the enumeration holds the shard lock, so it is
bounded on purpose, and a map too large to read in one response is a map
that wants `?f=` instead.

Expired fields are dropped as the walk meets them rather than reported.

### `POST /kkv/<key>`

Many fields under one hold of the lock, so the map is never seen half
written.  Body: the same frames `GET` answers with, the TTL optional.

    POST /kkv/user:1?kttl=86400
    4 5 60
    namealice
    3 2
    age30

`204` with `X-Kache-Count` and `X-Kache-Stored`, or `507` when the arena
refused some of them.  The body is parsed before anything is written, so
a malformed batch is rejected with `400` and changes nothing.

### `POST /kkvdel/<key>`

Many fields removed under one lock.  Body: `"<len>\n" <field bytes> "\n"`
per field.  `204` with `X-Kache-Removed`.

### `POST /kkvincr/<key>?f=<field>`, `POST /kkvdecr/<key>?f=<field>`

`/incr` on a field: reads it as a decimal integer, adds `?by=` (default
`1`), stores the result and returns it.  `?init=` is the value to start
from when the field is absent, so incrementing into an empty map creates
both.  `409` when the field is not an integer or the result would
overflow.  A request that carries no `?ttl=` leaves the field's expiry
alone.

### `POST /kkvtouch/<key>?f=<field>`

A new TTL for that field.  Without `?f=`, a new TTL for the map itself.
`204`, or `404`.

Queues
------

A deque with a TTL on the queue and another on every message.  Pushes go
to the right and pops come from the left by default, which makes the
plain calls a FIFO; `?side=l` or `?side=r` says otherwise.

| Parameter | Meaning |
| --- | --- |
| `?side=l\|r` | which end to work on |
| `?n=<count>` | how many messages, and frame the answer |
| `?ttl=`, `?ttlms=` | the message's expiry |
| `?kttl=`, `?kttlms=` | the queue's own expiry |
| `?maxlen=<n>` | after pushing, trim the far end to this many |

**A queue that is drained stops existing**, so a pop from an empty queue
and a pop from a queue that was never there answer alike: `204`.

A message that expires in the middle of a queue is skipped when read and
reclaimed when the messages in front of it are gone.  The layout that
makes a push cost no allocation is the same one that cannot cut a hole
in the middle; since messages are normally pushed with the same TTL,
expiry reaches the ends in order anyway.

### `POST /q/<key>`, `PUT /q/<key>`

Pushes the body as one message.  `201` when the queue was created, `204`
otherwise, with `X-Kache-Id` giving the message's id, `X-Kache-Count`
the new length and `X-Kache-Key-TTL` the queue's expiry.

### `POST /qpush/<key>`

Many messages under one hold of the lock.  Body: `"<valuelen> [ttl]\n"`
then that many raw bytes then a newline, per message.  `X-Kache-Stored`
says how many landed.

### `POST /qpop/<key>`

Without `?n=`: the single message's bytes as the body, with
`X-Kache-Id`, `X-Kache-TTL` and `X-Kache-Flags` as headers - which is
what a worker taking one job wants, and what `curl` can use without a
parser.

With `?n=<count>`: up to that many messages, framed as
`"<valuelen> <ttl> <id>\n" <bytes> "\n"`.

`204` when there was nothing to take.  At most `CFG_QPOP_MAX` (4096)
messages and 8 MiB come back at once; a message that will not fit stays
queued, because each one is taken only once the response has it.

### `GET /q/<key>`, `HEAD /q/<key>`

The same, without removing anything.  `?side=r` reads from the tail
backwards.  `HEAD` is how to ask a queue's length: `X-Kache-Count`.

### `POST /qmove/<key>?dst=<key>`

Takes one message off `<key>` and puts it on `?dst=`, under both shard
locks at once, and answers with its bytes.  `?from=` names the end it
comes off (default `l`) and `?to=` the end it goes on (default `r`).
`204` when the source is empty.

This is the reliable queue primitive: the message is in the source or in
the destination, never in neither, so a worker that dies after the move
leaves the job on the destination to be recovered rather than losing it.
It is the only call in kache that holds two locks, and it takes them in
address order, which is the whole of the deadlock story.

### `POST /qtrim/<key>?maxlen=<n>`

Keeps at most `n` messages, dropping from `?side=` (default `l`, the
oldest).  `204` with `X-Kache-Removed`.

### `POST /qtouch/<key>`

A new TTL for the queue itself.  `204`, or `404`.

### `DELETE /q/<key>`

Removes the queue, however long, in constant time.  `204`, or `404`.

Types
-----

A key holds a value, a map or a queue, and the wrong call on it answers
`409` rather than reinterpreting the bytes.  `DELETE /kv/<key>` is the
exception and removes any of the three: a key is a key.

Deferred reclamation
--------------------

Deleting a map of a hundred thousand fields, or a queue of a million
messages, is a constant time request.  The container leaves the index at
once - it is gone as far as any client can tell - and is taken apart a
few blocks at a time afterwards, by the requests that pass through that
shard and by a once a second housekeeping tick on each worker, which
takes the shard lock with a try and never waits for it.

`kache_reclaim_pending` in `/metrics` counts the containers still
waiting.  An allocation that cannot be satisfied drains the whole
backlog before it evicts anything a client can still read, so the memory
is never lost, only handed back late.


Housekeeping
------------

### `POST /flush`, `DELETE /flush`

Empties the store.  `204`.  Disabled unless the server was started with
`-F`, in which case it answers `403`.

### `GET /stats`

`text/plain`, one `name value` pair per line.

### `GET /metrics`

The same counters in Prometheus exposition format, prefixed `kache_`,
with a `# TYPE` line each.

### `GET /health`

`200 ok` as long as the server is accepting requests.

### `GET /`

A plain text summary of the above.

Minimal responses
-----------------

`-M` (`CFG_MINIMAL` at build time) drops the headers a cache client
rarely reads back.  `Server: kache` goes from every response;
`Connection: keep-alive` goes when the connection is being kept alive,
which is the HTTP/1.1 default and needs no saying, while
`Connection: close` is still always sent; and a successful `GET` of
`/kv/<key>` loses its `Content-Type` along with the `ETag`,
`X-Kache-TTL` and `X-Kache-Flags` trio.  A `HEAD` of the same key keeps
all of them: it returns no body, so there is nothing for the elision to
save, and metadata is the whole reason to ask.  Nothing else moves.
`Date` and
`Content-Length` stay, every other endpoint keeps its `Content-Type`,
and the status codes are the ones they always were.

Writes keep their metadata.  `PUT`, `POST`, `DELETE`, `/incr`,
`/append`, `/touch` and the batch endpoints lose only the two headers
every response loses, never the `ETag`, `X-Kache-TTL` and
`X-Kache-Flags`, so the token a compare and swap needs is still handed
back by the write that minted it, and `If-Match` and `If-None-Match`
work as they always did.  Only the read side goes quiet.

It never applies to an HTTP/1.0 client.  Such a client keeps the
connection only when the server echoes `Connection: keep-alive`, and
that echo is the thing minimal mode stops sending, so a request that
announces `HTTP/1.0` gets the full header set whatever the server was
started with.

What it buys is bytes: for a 64 byte value the header block goes from
201 bytes down to 76, and the whole response from 265 to 140.  What it
costs is the read side metadata, so `-M` suits a client that gets a
value and wants the bytes, and does not suit one that reads a key to
learn its `ETag` or its remaining TTL - though such a client can use
`HEAD`, which keeps them.

Status codes
------------

| Code | Meaning |
| --- | --- |
| `200` | value or document returned |
| `201` | key created |
| `204` | done, nothing to return |
| `400` | malformed request, key or parameter |
| `403` | `/flush` without `-F` |
| `404` | no such key or endpoint |
| `405` | method not allowed on that endpoint |
| `409` | value is not an integer, the result overflowed, or the key holds another type |
| `412` | a precondition on `If-Match` or `If-None-Match` failed |
| `413` | key or value over the configured limit |
| `414` | key longer than the limit |
| `431` | request headers too large |
| `501` | chunked transfer encoding |
| `505` | not HTTP/1.0 or HTTP/1.1 |
| `507` | the shard could not free enough space for the value |

`507` is worth a word: it means the target shard could not assemble a
contiguous run for this value even after evicting.  On a container it
also means the container has reached the size of its shard's arena - the
file divided by the shard count - since a map or a queue lives in one
shard entirely.  Either way nothing changed, and `X-Kache-Stored` says
how much of a batch did land.  In practice it needs
a value close to the whole per shard arena, which is what the geometry
check at startup is there to prevent.

Examples
--------

    # store with a five minute ttl
    curl -X PUT -d 'session data' 'localhost:7070/kv/sess/abc?ttl=300'

    # compare and swap
    etag=$(curl -sI localhost:7070/kv/sess/abc | sed -n 's/^ETag: //p' | tr -d '\r')
    curl -X PUT -H "If-Match: $etag" -d 'new data' localhost:7070/kv/sess/abc

    # a map with a day on the key and a minute on one of its fields
    curl -X PUT -d alice 'localhost:7070/kkv/user:1?f=name&kttl=86400'
    curl -X PUT -d 'live-token' 'localhost:7070/kkv/user:1?f=tok&ttl=60'
    curl 'localhost:7070/kkv/user:1'

    # a work queue, and a worker taking one job reliably
    curl -X POST -d 'job payload' localhost:7070/q/work
    curl -X POST 'localhost:7070/qmove/work?dst=work:inflight'

    # under -M a read carries no ETag, so keep the one the write gave back
    etag=$(curl -s -D - -o /dev/null -X PUT -d 'session data' \
           localhost:7070/kv/sess/abc | sed -n 's/^ETag: //p' | tr -d '\r')

    # take a lock that expires by itself
    curl -f -X PUT -H 'If-None-Match: *' -d held 'localhost:7070/kv/lock?ttl=30'

    # a counter
    curl -X POST 'localhost:7070/incr/hits?by=1'
