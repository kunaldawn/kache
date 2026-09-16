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

Endpoints
---------

### `GET /kv/<key>` and `HEAD /kv/<key>`

`200` with the value as `application/octet-stream`, or `404`.  `HEAD`
sends the same headers, `Content-Length` included, and no body.

### `PUT /kv/<key>`, `POST /kv/<key>`

Stores the request body.  `201` when the key was created, `204` when it
replaced a value.  Conditional forms:

| Request header | Meaning | On failure |
| --- | --- | --- |
| `If-None-Match: *` | store only if the key is absent | `412` |
| `If-Match: *` | store only if the key is present | `412` |
| `If-Match: "<etag>"` | store only if the value is unchanged | `412` |

A compare and swap is therefore a `GET` followed by a `PUT` carrying the
`ETag` you read, and `412` means someone got there first.

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
| `409` | value is not an integer, or the result overflowed |
| `412` | a precondition on `If-Match` or `If-None-Match` failed |
| `413` | key or value over the configured limit |
| `414` | key longer than the limit |
| `431` | request headers too large |
| `501` | chunked transfer encoding |
| `505` | not HTTP/1.0 or HTTP/1.1 |
| `507` | the shard could not free enough space for the value |

`507` is worth a word: it means the target shard could not assemble a
contiguous run for this value even after evicting.  In practice it needs
a value close to the whole per shard arena, which is what the geometry
check at startup is there to prevent.

Examples
--------

    # store with a five minute ttl
    curl -X PUT -d 'session data' 'localhost:7070/kv/sess/abc?ttl=300'

    # compare and swap
    etag=$(curl -sI localhost:7070/kv/sess/abc | sed -n 's/^ETag: //p' | tr -d '\r')
    curl -X PUT -H "If-Match: $etag" -d 'new data' localhost:7070/kv/sess/abc

    # take a lock that expires by itself
    curl -f -X PUT -H 'If-None-Match: *' -d held 'localhost:7070/kv/lock?ttl=30'

    # a counter
    curl -X POST 'localhost:7070/incr/hits?by=1'
