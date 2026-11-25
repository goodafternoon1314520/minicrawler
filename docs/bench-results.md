# Benchmark results

- date: 2026-11-25
- cpu: Intel(R) Core(TM) i7-14650HX (8 threads)
- kernel: Linux 6.12.0-275.el10.x86_64
- commit: 8f886d8
- target: local Python slow_site.py, 0.1s per response, over loopback
- caveat: client and server share one machine; numbers show scaling shape, not absolute capacity

## A. Concurrency scaling (1 process, in-memory queue, 300 pages)

| concurrency | pages/s | p50 (ms) | p95 (ms) |
|---:|---:|---:|---:|
| 1 | 625.267 | 1.0 | 3.3 |
| 10 | 29.5811 | 6.0 | 1022.8 |
| 50 | 24.4293 | 41.5 | 1264.7 |
| 200 | 21.2163 | 11.9 | 1457.2 |

## B. Multi-process scaling (Redis queue, 10 concurrent requests per process, 1200 pages)

| processes | wall time (s) | pages/s (server side) |
|---:|---:|---:|
| 1 | 1.1 | 10.0 |
| 2 | 1.1 | 9.7 |
| 3 | 1.1 | 9.7 |

## C. Rate limiter accuracy (1 process, in-memory limiter, burst 1, 200 pages)

| --rate | measured at server (req/s) |
|---:|---:|
| 10 | n/a |
| 20 | n/a |
| 50 | n/a |

## D. Global rate limit across 3 processes (--rate 30, burst 1, 300 pages)

| limiter | measured at server (req/s) |
|---|---:|
| shared (Redis token bucket / per-process) | n/a |
| local (Redis token bucket / per-process) | n/a |

