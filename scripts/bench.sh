#!/usr/bin/env bash
# 对本机慢速站点跑四组实验，markdown 结果输出到 stdout，进度输出到 stderr。
# 用法:
#   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
#   ./scripts/bench.sh | tee docs/bench-results.md
# 需要本机 Redis（无密码）；没有 redis-cli/valkey-cli 时会跳过需要 Redis 的两组。
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./build-release/minicrawler}
PORT=${PORT:-8000}
DELAY=${DELAY:-0.1}
SITE_LOG=/tmp/mc_bench_site.log
REDIS_CLI=$(command -v redis-cli || command -v valkey-cli || true)
SITE_PID=""

log() { echo "[bench] $*" >&2; }

stop_site() {
  if [[ -n "$SITE_PID" ]]; then
    kill "$SITE_PID" 2>/dev/null || true
    wait "$SITE_PID" 2>/dev/null || true
    SITE_PID=""
  fi
}
trap stop_site EXIT

start_site() {  # $1 = 页面数。每次重启，让服务端日志只包含当前实验
  stop_site
  LOG=1 python3 scripts/slow_site.py "$1" "$DELAY" "$PORT" > "$SITE_LOG" 2>&1 &
  SITE_PID=$!
  for _ in $(seq 50); do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/robots.txt"; then return 0; fi
    sleep 0.1
  done
  echo "site failed to start" >&2
  exit 1
}

reset_redis() { "$REDIS_CLI" --scan --pattern 'mc:*' | xargs -r "$REDIS_CLI" del > /dev/null; }

run_crawl() { "$BIN" "http://127.0.0.1:$PORT/" --delay-ms 0 --max-pages 100000 "$@"; }

# 服务端收到的（非 robots）请求数
site_requests() { grep -vc robots "$SITE_LOG" || true; }

# 服务端视角的平均请求速率：(n-1) / (最后一个请求时刻 - 第一个请求时刻)
measure_rate() {
  grep -v robots "$SITE_LOG" |
    awk 'NR==1{f=$1} {l=$1; n++} END{ if (n>1 && l>f) printf "%.1f", (n-1)/(l-f); else print "n/a" }'
}

echo "# Benchmark results"
echo
echo "- date: $(date -u +%F)"
echo "- cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -1) ($(nproc) threads)"
echo "- kernel: $(uname -sr)"
echo "- commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "- target: local Python slow_site.py, ${DELAY}s per response, over loopback"
echo "- caveat: client and server share one machine; numbers show scaling shape, not absolute capacity"

# ---------- A. 并发扩展 ----------
echo
echo "## A. Concurrency scaling (1 process, in-memory queue, 300 pages)"
echo
echo "| concurrency | pages/s | p50 (ms) | p95 (ms) |"
echo "|---:|---:|---:|---:|"
for c in 1 10 50 200; do
  log "A: concurrency=$c"
  start_site 300
  out=$(run_crawl --concurrency "$c" --per-host "$c")
  pps=$(grep -oE '[0-9.e+]+ pages/s' <<<"$out" | head -1 | cut -d' ' -f1)
  p50=""; p95=""
  read -r p50 p95 <<<"$(sed -nE 's/^latency: p50=([0-9.]+)ms p95=([0-9.]+)ms.*/\1 \2/p' <<<"$out")" || true
  echo "| $c | ${pps:-n/a} | ${p50:-n/a} | ${p95:-n/a} |"
done

# ---------- B. 多进程扩展 ----------
echo
echo "## B. Multi-process scaling (Redis queue, 10 concurrent requests per process, 1200 pages)"
echo
if [[ -z "$REDIS_CLI" ]]; then
  echo "_skipped: no redis-cli / valkey-cli found_"
else
  echo "| processes | wall time (s) | pages/s (server side) |"
  echo "|---:|---:|---:|"
  for n in 1 2 3; do
    log "B: processes=$n"
    reset_redis
    start_site 1200
    pids=()
    t0=$(date +%s.%N)
    for i in $(seq "$n"); do
      run_crawl --redis --worker-id "w$i" --concurrency 10 --per-host 10 > "/tmp/mc_bench_w$i.log" &
      pids+=($!)
    done
    wait "${pids[@]}"
    t1=$(date +%s.%N)
    awk -v n="$(site_requests)" -v a="$t0" -v b="$t1" -v p="$n" \
      'BEGIN{ printf "| %d | %.1f | %.1f |\n", p, b-a, n/(b-a) }'
  done
fi

# ---------- C. 单进程限速精度 ----------
echo
echo "## C. Rate limiter accuracy (1 process, in-memory limiter, burst 1, 200 pages)"
echo
echo "| --rate | measured at server (req/s) |"
echo "|---:|---:|"
for rate in 10 20 50; do
  log "C: rate=$rate"
  start_site 200
  run_crawl --rate "$rate" --burst 1 --concurrency 20 --per-host 20 > /dev/null
  echo "| $rate | $(measure_rate) |"
done

# ---------- D. 跨进程全局限速 ----------
echo
echo "## D. Global rate limit across 3 processes (--rate 30, burst 1, 300 pages)"
echo
if [[ -z "$REDIS_CLI" ]]; then
  echo "_skipped: no redis-cli / valkey-cli found_"
else
  echo "| limiter | measured at server (req/s) |"
  echo "|---|---:|"
  for mode in shared local; do
    log "D: limiter=$mode"
    reset_redis
    start_site 300
    extra=()
    if [[ "$mode" == local ]]; then extra=(--local-limit); fi
    pids=()
    for i in 1 2 3; do
      run_crawl --redis "${extra[@]}" --worker-id "w$i" --rate 30 --burst 1 \
        --concurrency 10 --per-host 10 > /dev/null &
      pids+=($!)
    done
    wait "${pids[@]}"
    echo "| $mode (Redis token bucket / per-process) | $(measure_rate) |"
  done
fi
echo