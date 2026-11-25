# minicrawler

A concurrent, polite web crawler in C++17, built from raw sockets up: epoll event loop,
hand-written HTTP/1.1 client, RESP (Redis protocol) client, robots.txt parser, and a
distributed task queue on Redis. It was built step by step as a learning project, and the
design notes in `docs/` explain every decision and trade-off.

![CI](https://github.com/<your-name>/minicrawler/actions/workflows/ci.yml/badge.svg)

> **Scope warning:** it speaks plain `http://` only. There is no TLS yet, so it cannot crawl
> most of the public web. See [Known limitations](#known-limitations).

## Features

- **Event-driven networking**: one epoll loop drives hundreds of non-blocking connections
  (state machine per connection: connecting → sending → receiving), with per-request timeouts.
- **Reliable distributed queue**: Redis lists with atomic `RPOPLPUSH`, Lua scripts for
  dedup + enqueue, worker heartbeats (TTL keys) so a crashed worker's tasks are reclaimed.
- **Polite by construction**: robots.txt (groups, longest-match, `*`/`$`, `Crawl-delay`,
  RFC 9309 failure semantics), per-host concurrency/interval limits, and a **token bucket
  shared by all processes** (Lua on Redis, using the Redis clock).
- **Retries**: failures are classified (retry 503/timeouts, don't retry 404/NXDOMAIN),
  exponential backoff with jitter, `Retry-After` honored, delayed queue in a Redis ZSET.
- **Persistence**: pages stored in MySQL through prepared statements, idempotent upserts,
  crash recovery (`in_progress` → `pending`).
- **Observability**: per-attempt status histogram, latency percentiles, periodic progress line.
- **Tested**: unit tests, integration tests against in-process fake servers and a real
  Redis/MySQL, run under ASan, UBSan and TSan in CI.

## Architecture

```
                    ┌───────────────── one crawler process ─────────────────┐
                    │  event loop thread                 storage thread     │
 seed URL ─────────►│  epoll ─ async HTTP client  ◄─┐   (owns MySQL + queue │
                    │  robots check ─ rate limiter  │    connections)       │
                    │  HTML link extraction         └── command queue ─────►│──► MySQL (pages)
                    └───────────────────────┬───────────────────────────────┘
                                            │ (pop / push / complete / retry)
                              ┌─────────────▼──────────────┐
   other crawler processes ──►│ Redis: seen, pending, proc │
                              │ delayed, token buckets,    │
                              │ robots cache, heartbeats   │
                              └────────────────────────────┘
```

More detail (threading model, Redis keys, task lifecycle): [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build

Requirements: CMake ≥ 3.16, a C++17 compiler, `pkg-config`, and the MySQL client library
(`libmysqlclient-dev` on Debian/Ubuntu, `mysql-devel` or `mariadb-connector-c-devel` on RHEL-likes).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Debug builds enable AddressSanitizer and UBSan. `-DMC_TSAN=ON` builds with ThreadSanitizer
instead. `-DMC_BUILD_TESTS=OFF` skips the tests (and the GoogleTest download).

## Run

```bash
# Local site to practice on (600 pages, 0.2 s per response)
python3 scripts/slow_site.py 600 0.2 &

# Single process, in-memory queue
./build/minicrawler http://127.0.0.1:8000/ --delay-ms 0 --concurrency 20 --per-host 20 --max-pages 1000

# Several processes sharing one queue and one global rate limit
export MC_REDIS_HOST=127.0.0.1
for i in 1 2 3; do
  ./build/minicrawler http://127.0.0.1:8000/ --redis --worker-id w$i --rate 20 \
      --delay-ms 0 --concurrency 10 --per-host 10 --max-pages 100000 &
done; wait
```

### With Docker Compose (Redis + MySQL + demo site + 3 crawlers)

```bash
cp .env.example .env            # then fill in the two passwords
docker compose build
docker compose up -d --wait redis mysql site
docker compose --profile crawl up --no-deps crawler1 crawler2 crawler3
docker compose exec redis redis-cli get mc:n_done
docker compose down -v          # also wipes the dedup set, so the next run crawls again
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `--depth N` | 2 | Max link depth (seed = 0) |
| `--max-pages N` | 50 | Max tasks taken from the queue in this run (retries count) |
| `--concurrency N` | 16 | Requests in flight, in total |
| `--per-host N` | 8 | Requests in flight per host (**raise it together with `--concurrency`**) |
| `--delay-ms N` | 500 | Min interval between request starts per host, per process |
| `--rate R` / `--burst B` | off / 1 | Token bucket per host, shared across processes with `--redis` |
| `--timeout S` | 10 | Per-request timeout |
| `--retries N` / `--retry-base-ms` / `--retry-max-ms` | 3 / 1000 / 60000 | Retry policy |
| `--no-robots` | off | Ignore robots.txt (only for sites you own) |
| `--max-crawl-delay S` | 30 | Cap on a site's `Crawl-delay` |
| `--any-host` | off | Follow links to other hosts |
| `--mysql` | off | Store pages in MySQL (also the queue unless `--redis`) |
| `--redis`, `--worker-id`, `--ns`, `--hb-ttl` | off, `w1`, `mc`, 15 | Shared queue; each process needs a unique worker id |
| `--local-limit` | off | Keep the rate limiter in-process even with `--redis` (for experiments) |
| `--progress S` | 0 | Print a stats line to stderr every S seconds |
| `--stats` | | Print queue counters and exit |

Environment: `MC_DB_HOST MC_DB_PORT MC_DB_USER MC_DB_PASS MC_DB_NAME`,
`MC_REDIS_HOST MC_REDIS_PORT MC_REDIS_PASS`. Credentials are never taken from the command line.

## Testing

```bash
ctest --test-dir build --output-on-failure                      # no external services needed
MC_TEST_REDIS=1 ctest --test-dir build --output-on-failure      # + Redis integration tests
MC_TEST_DB=minicrawler_test ctest --test-dir build -R Mysql     # + MySQL tests (db name must end in _test)
```

The MySQL tests truncate tables, so they refuse to run on any database whose name doesn't end in `_test`.

## Benchmarks

`scripts/bench.sh` runs four experiments against a local Python site and measures request rates
**at the server**. Results from my machine: [docs/bench-results.md](docs/bench-results.md).

<!-- Paste 3-4 headline numbers here from YOUR run, e.g. the concurrency curve and the
     shared-vs-local limiter result. Do not copy numbers from anywhere else. -->

These are loopback numbers against a Python server: they show scaling behavior, not real-world capacity.

## Politeness and ethics

Only crawl sites you own or have permission to crawl. The crawler obeys robots.txt by default,
identifies itself as `minicrawler/0.1`, and defaults to a 500 ms interval per host.
`--no-robots` exists for testing against your own servers.

## Known limitations

- **No HTTPS.** Needs non-blocking TLS (e.g. OpenSSL in the epoll loop). Also no gzip/deflate
  and no HTTP keep-alive (one connection per request).
- `getaddrinfo` is blocking (results are cached per host); Redis calls for the rate limiter and
  robots cache run on the event loop thread (dedicated connection, usually sub-millisecond).
- The command queue to the storage thread is unbounded: a slow database means growing memory
  instead of back-pressure.
- Retries are supported by the in-memory and Redis queues, not by the MySQL queue.
  A robots.txt 5xx marks the affected tasks failed instead of retrying them.
- URL normalization is basic: `/a` vs `/a/`, query-parameter order and percent-encoding
  differences are not merged. The dedup set stores SHA-1 hashes and never expires.
- HTML link extraction is a hand-written tolerant scanner, not a spec-compliant parser; no `<base>` support.
- robots.txt: no percent-encoding normalization, `Request-rate` and `Sitemap` are ignored.
- Latency samples are kept in a vector (fine for tests and benchmarks, not for multi-day runs).

## License

MIT, see [LICENSE](LICENSE).