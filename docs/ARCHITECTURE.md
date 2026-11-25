# Architecture

## Threads (per process)

| Thread | Owns | Never does |
|---|---|---|
| event loop (main) | epoll, all sockets, HTML parsing, the robots/rate-limiter Redis connection | MySQL calls, queue operations |
| storage worker | MySQL connection, queue connection (Redis or MySQL) | touching sockets |

The two talk through a mutex-protected command queue (event loop → worker) and a prefetch
queue plus an `eventfd` wake-up (worker → event loop). Because each connection has exactly one
owner thread, the database objects need no locks. Commands are FIFO, which keeps the order
"save page → enqueue links → mark complete" that makes crashes safe.

## Task lifecycle (at-least-once)

    push ─(Lua: SADD seen, then LPUSH)─► pending ─RPOPLPUSH─► proc:<worker> ─(Lua: LREM + INCR)─► done / failed
                                            ▲                       │ failure worth retrying
                                            └─(Lua: ZREM+LPUSH)─ delayed ◄─(Lua: LREM + ZADD)─┘

A task is always in exactly one list. A crash can make a task run twice, never zero times;
page upserts and the `seen` set make the repeat harmless. A worker whose heartbeat key expired
has its `proc:<id>` list moved back to `pending` by any other worker.

## Per-request pipeline (event loop)

1. robots.txt for the host known? If not, fetch it (shared cache in Redis) and keep the task waiting.
2. Path disallowed → skip without a request.
3. Per-host in-flight limit and local interval.
4. Global token bucket (Lua on Redis; Redis `TIME` is the only clock). Not granted → remember "not before".
5. Send request. Redirect targets re-enter the queue as new tasks (so steps 1–4 apply to them too).
6. Classify the outcome: success / retryable failure / permanent failure.

Order matters: tasks that can't be sent must not hold a concurrency slot, and a token must not
be spent when the request can't start.

## Redis keys (`<ns>` defaults to `mc`)

| Key | Type | Content |
|---|---|---|
| `<ns>:seen` | SET | `sha1(url)` of every URL ever enqueued or marked seen |
| `<ns>:pending` | LIST | `"depth/attempt url"` (old `"depth url"` still parses) |
| `<ns>:proc:<worker>` | LIST | tasks currently held by a worker |
| `<ns>:delayed` | ZSET | retry tasks, score = due time in Redis ms |
| `<ns>:workers` / `<ns>:hb:<worker>` | SET / STRING+TTL | registered workers / liveness |
| `<ns>:n_done`, `<ns>:n_failed`, `<ns>:errors` | STRING, STRING, HASH | counters; url → last error |
| `<ns>:robots:<host:port>` | STRING+TTL | `A` allow-all / `D` disallow-all / `T`+file body |
| `<ns>:rl:<host:port>` | HASH+TTL | token bucket state (`tokens`, `ts`) |

## MySQL tables

- `crawl_tasks(url_hash BINARY(16) UNIQUE, url, depth, status, last_error, ...)`, index `(status, id)`.
- `pages(url_hash BINARY(16) UNIQUE, url, depth, status_code, content_type, body_size, body MEDIUMBLOB, fetched_at)`.

URLs are `ascii`, bodies are raw bytes; the unique key is a hash because a 2 KB URL doesn't fit
an InnoDB index. All statements are prepared (no string concatenation of untrusted data).

## Failure policy

| Outcome | Action |
|---|---|
| timeout, connection refused/reset, truncated response | retry with backoff |
| 408 425 429 500 502 503 504 | retry; honor `Retry-After`; 429/503 also back off the whole host |
| other 4xx, 3xx without target | final, counted as a successful exchange |
| NXDOMAIN, `response too large`, non-http scheme | final failure |
| robots.txt 4xx | allow all (24 h) |
| robots.txt 5xx / unreachable | disallow all (5 min) |
| limiter or Redis errors | fail closed (wait), never fail open |