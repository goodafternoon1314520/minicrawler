#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <iomanip>

#include "crawler/async_crawler.h"
#include "redis/redis_client.h"
#include "redis/redis_frontier.h"
#include "redis/redis_politeness.h"
#include "storage/mysql_db.h"
#include "storage/mysql_store.h"
#include "util/url.h"

namespace {

    struct Cli {
        std::string seed;
        bool mysql = false;
        bool redis = false;
        bool stats = false;
        bool local_limit = false;
        std::string worker_id = "w1";
        std::string ns = "mc";
        int hb_ttl = 15;
        mc::CrawlOptions opt;
    };

    void usage() {
        std::cerr << "usage: minicrawler <seed-url> [--depth N] [--max-pages N] [--delay-ms N]\n"
                     "                              [--retries N] [--retry-base-ms N] [--retry-max-ms N] [--progress S]\n"
                     "                              [--concurrency N] [--per-host N] [--timeout S]\n"
                     "                              [--rate R] [--burst B] [--max-crawl-delay S] [--no-robots]\n"
                     "                              [--any-host] [--mysql] [--redis]\n"
                     "                              [--worker-id NAME] [--ns NAME] [--hb-ttl S] [--local-limit]\n"
                     "       minicrawler --stats [--redis]   (show task counts)\n"
                     "  --rate R         max requests/second per host, summed over ALL processes (needs --redis to be\n"
                     "                   shared; otherwise it is per process). 0 = off\n"
                     "  --burst B        token bucket size (default 1)\n"
                     "  --no-robots      do not fetch or obey robots.txt (only for sites you own)\n"
                     "  --local-limit    keep the rate limiter in this process even with --redis (for experiments)\n"
                     "env: MC_DB_HOST MC_DB_PORT MC_DB_USER MC_DB_PASS MC_DB_NAME\n"
                     "     MC_REDIS_HOST MC_REDIS_PORT MC_REDIS_PASS\n";
    }

    bool to_long(const char* s, long* out) {
        char* end = nullptr;
        long v = std::strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0') return false;
        *out = v;
        return true;
    }

    bool to_double(const char* s, double* out) {
        char* end = nullptr;
        double v = std::strtod(s, &end);
        if (*s == '\0' || *end != '\0' || !std::isfinite(v) || v < 0) return false;
        *out = v;
        return true;
    }

    bool valid_name(const std::string& s) {
        if (s.empty() || s.size() > 32) return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    }

    bool parse_args(int argc, char** argv, Cli* cli) {
        auto& o = cli->opt;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            long v = 0;
            double d = 0;
            if (a == "--any-host") {
                o.same_host_only = false;
            } else if (a == "--mysql") {
                cli->mysql = true;
            } else if (a == "--redis") {
                cli->redis = true;
            } else if (a == "--stats") {
                cli->stats = true;
            } else if (a == "--no-robots") {
                o.obey_robots = false;
            } else if (a == "--local-limit") {
                cli->local_limit = true;
            } else if ((a == "--worker-id" || a == "--ns") && i + 1 < argc && valid_name(argv[i + 1])) {
                (a == "--worker-id" ? cli->worker_id : cli->ns) = argv[++i];
            } else if ((a == "--rate" || a == "--burst" || a == "--max-crawl-delay") && i + 1 < argc &&
                       to_double(argv[i + 1], &d)) {
                ++i;
                if (a == "--rate") o.host_rate = d;
                else if (a == "--burst") o.host_burst = std::max(1.0, d);
                else o.max_crawl_delay = d;
            } else if ((a == "--depth" || a == "--max-pages" || a == "--delay-ms" || a == "--concurrency" ||
                        a == "--per-host" || a == "--timeout" || a == "--hb-ttl" || a == "--retries" ||
                        a == "--retry-base-ms" || a == "--retry-max-ms" || a == "--progress") &&
                       i + 1 < argc && to_long(argv[i + 1], &v) && v >= 0) {
                ++i;
                if (a == "--depth") o.max_depth = static_cast<int>(v);
                else if (a == "--max-pages") o.max_pages = static_cast<size_t>(v);
                else if (a == "--delay-ms") o.delay_ms = static_cast<int>(v);
                else if (a == "--concurrency") o.concurrency = static_cast<int>(std::max(1L, v));
                else if (a == "--per-host") o.max_per_host = static_cast<int>(std::max(1L, v));
                else if (a == "--timeout") o.timeout_sec = static_cast<int>(std::max(1L, v));
                else if (a == "--retries") o.max_retries = static_cast<int>(v);
                else if (a == "retry-base-ms") o.retry_base_ms = static_cast<int>(v);
                else if (a == "retry-max-ms") o.retry_max_ms = static_cast<int>(std::max(1L, v));
                else if (a == "progress") o.progress_sec = static_cast<int>(v);
                else cli->hb_ttl = static_cast<int>(std::max(1L, v));
            } else if (!a.empty() && a[0] != '-' && cli->seed.empty()) {
                cli->seed = a;
            } else {
                return false;
            }
        }
        return cli->stats || !cli->seed.empty();
    }

    void print_counts(const mc::TaskCounts& c) {
        std::cout << "tasks: pending=" << c.pending << "delayed=" << c.delayed << " in_progress=" << c.in_progress
                  << " done=" << c.done << " failed=" << c.failed << "\n";
    }

}  // namespace

int main(int argc, char** argv) {
    Cli cli;
    if (!parse_args(argc, argv, &cli)) { usage(); return 1; }
    if (cli.stats && !cli.redis) cli.mysql = true;

    // ---- MySQL：页面存储（以及没有 --redis 时的任务队列）----
    mc::Db db;
    std::unique_ptr<mc::MysqlFrontier> mysql_frontier;
    std::unique_ptr<mc::MysqlPageSink> sink;
    if (cli.mysql) {
        std::string err;
        if (!db.connect(mc::DbConfig::from_env(), &err)) {
            std::cerr << "mysql connect failed: " << err << "\n";
            return 2;
        }
        if (!mc::ensure_schema(db, &err)) {
            std::cerr << "create schema failed: " << err << "\n";
            return 2;
        }
        mysql_frontier = std::make_unique<mc::MysqlFrontier>(db);
        sink = std::make_unique<mc::MysqlPageSink>(db);
    }

    // ---- Redis：共享任务队列（存储线程专用连接）----
    mc::RedisClient redis_client;
    std::unique_ptr<mc::RedisFrontier> redis_frontier;
    if (cli.redis) {
        std::string err;
        if (!redis_client.connect(mc::RedisConfig::from_env(), &err)) {
            std::cerr << "redis connect failed: " << err << "\n";
            return 2;
        }
        mc::RedisFrontierOptions ro;
        ro.ns = cli.ns;
        ro.worker_id = cli.worker_id;
        ro.hb_ttl_sec = cli.hb_ttl;
        redis_frontier = std::make_unique<mc::RedisFrontier>(redis_client, ro);
    }

    if (cli.stats) {
        print_counts(redis_frontier ? redis_frontier->counts() : mysql_frontier->counts());
        return 0;
    }

    auto seed = mc::parse_url(cli.seed);
    if (!seed) { std::cerr << "invalid seed url\n"; return 1; }

    // ---- Redis：限速器和 robots 缓存（事件循环线程专用连接，和上面那条不能共用）----
    mc::RedisClient redis_policy_client;
    std::unique_ptr<mc::RedisRateLimiter> redis_limiter;
    std::unique_ptr<mc::RedisRobotsStore> redis_robots;
    if (cli.redis) {
        std::string err;
        if (!redis_policy_client.connect(mc::RedisConfig::from_env(), &err)) {
            std::cerr << "redis connect failed: " << err << "\n";
            return 2;
        }
        redis_robots = std::make_unique<mc::RedisRobotsStore>(redis_policy_client, cli.ns);
        if (!cli.local_limit) redis_limiter = std::make_unique<mc::RedisRateLimiter>(redis_policy_client, cli.ns);
    }
    mc::Politeness pol;
    pol.robots = redis_robots.get();
    pol.limiter = redis_limiter.get();

    // ---- 选定任务队列并恢复上次遗留的任务 ----
    mc::Frontier* frontier = nullptr;
    if (redis_frontier) {
        std::string err;
        if (!redis_frontier->register_worker(&err)) {
            std::cerr << "register worker failed: " << err << "\n";
            return 2;
        }
        if (size_t n = redis_frontier->recover()) std::cout << "recovered " << n << " interrupted task(s)\n";
        frontier = redis_frontier.get();
    } else if (mysql_frontier) {
        if (size_t n = mysql_frontier->recover()) std::cout << "recovered " << n << " interrupted task(s)\n";
        frontier = mysql_frontier.get();
    }

    const auto t0 = std::chrono::steady_clock::now();
    // run() 期间：db 和 redis_client 只被存储线程使用；redis_policy_client 只被本线程（事件循环）使用
    mc::AsyncCrawler crawler(cli.opt, frontier, sink.get(), pol);
    auto stats = crawler.run(*seed, [](const mc::PageResult& r) {
        if (r.skipped) {
            std::cout << "[d=" << r.depth << "] SKIP " << r.url << "  (" << r.error << ")\n";
        } else if (r.retrying) {
            std::cout << "[d=" << r.depth << "] RETRY " << r.url << " (" << r.error << ", attempt "
                      << r.attempt + 1 << ", in " << r.retry_in_ms << "ms)\n";
        }else if (!r.error.empty()) {
            std::cout << "[d=" << r.depth << "] ERR  " << r.url << "  (" << r.error << ")\n";
        } else {
            std::cout << "[d=" << r.depth << "] " << r.status << "  " << r.url << "  (" << r.bytes << " bytes, "
                      << r.links_found << " links)\n";
        }
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (redis_frontier) redis_frontier->unregister_worker();

    const size_t total = stats.fetched + stats.failed;
    std::cout << "\ndone";
    if (redis_frontier) std::cout << " [worker " << cli.worker_id << "]";
    std::cout << ": fetched=" << stats.fetched << " failed=" << stats.failed << " retried=" << stats.retried
              << " skipped_robots=" << stats.skipped_robots << " robots_fetched=" << stats.robots_fetched
              << " skipped_non_http=" << stats.skipped_unsupported << " store_failed=" << stats.store_failed << "\n"
              << "peak_in_flight=" << stats.peak_in_flight << "  elapsed=" << secs << "s  "
              << (secs > 0 ? total / secs : 0.0) << " pages/s  bytes=" << stats.bytes << "\n";
    std::cout << "status (per attempt, 0 = no response):";
    for (const auto& [code, n] : stats.by_status) std::cout << " " << code << "=" << n;
    std::cout << "\n"
              << std::fixed << std::setprecision(1) << "latency: p50=" << stats.latency_p50_ms
              << "ms p95=" << stats.latency_p95_ms << "ms max=" << stats.latency_max_ms << "ms\n";
    if (redis_frontier) print_counts(redis_frontier->counts());
    else if (mysql_frontier) print_counts(mysql_frontier->counts());
}