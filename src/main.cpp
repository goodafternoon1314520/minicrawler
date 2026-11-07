//
// Created by davidcopperfield on 26-10-15.
//
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <string>
#include <memory>
#include <cctype>

#include "crawler/async_crawler.h"
#include "storage/mysql_store.h"
#include "storage/mysql_db.h"
#include "util/url.h"
#include "redis/redis_client.h"
#include "redis/redis_frontier.h"

namespace {
    struct Cli {
        std::string seed;
        bool mysql = false;
        bool stats = false;
        bool redis = false;
        std::string worker_id = "w1";
        std::string ns = "mc";
        int hb_ttl = 15;
        mc::CrawlOptions opt;
    };

    void usage() {
        std::cerr << "usage: minicrawler <seed-url> [--depth N] [--max-pages N] [--delay-ms N]\n"
                     "                              [--concurrency N] [--per-host N] [--timeout S]\n"
                     "                              [--any-host] [--mysql] [--redis]\n"
                     "                              [--worker-id NAME] [--ns NAME] [--hb-ttl S]\n"
                     "       minicrawler --stats [--redis]   (show task counts)\n"
                     "  --mysql          store pages in MySQL (and use MySQL as the queue unless --redis)\n"
                     "  --redis          use Redis as the shared task queue (multi-process crawling)\n"
                     "env: MC_DB_HOST MC_DB_PORT MC_DB_USER MC_DB_PASS MC_DB_NAME\n"
                     "     MC_REDIS_HOST MC_REDIS_PORT MC_REDIS_PASS\n";
    }

    bool to_long(const char *s, long *out) {
        char *end = nullptr;
        long v = std::strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0')
            return false;
        *out = v;
        return true;
    }

    bool valid_name(const std::string& s) {
        if (s.empty() || s.size() > 32)
            return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
    }

    bool parse_args(int argc, char** argv, Cli* cli) {
        auto& o = cli->opt;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            long v = 0;
            if (a == "--any-host") {
                o.same_host_only = false;
            }
            else if (a == "--mysql") {
                cli->mysql = true;
            }
            else if (a == "--redis") {
                cli->redis = true;
            }
            else if (a == "--stats") {
                cli->stats = cli->mysql = true;
            }
            else if ((a == "-depth" || a == "--ns") && i + 1 < argc && valid_name(argv[i + 1])) {
                (a == "--worker_id" ? cli->worker_id : cli->ns) = argv[++i];
            }
            else if ((a == "--depth" || a == "--max-pages" || a == "--delay-ms" ||
                        a == "--concurrency" || a == "--per-host" || a == "--timeout" || a == "--hb-ttl") &&
                       i + 1 < argc && to_long(argv[i + 1], &v) && v >= 0) {
                ++i;
                if (a == "--depth") o.max_depth = static_cast<int>(v);
                else if (a == "--max-pages") o.max_pages = static_cast<size_t>(v);
                else if (a == "--delay-ms") o.delay_ms = static_cast<int>(v);
                else if (a == "--concurrency") o.concurrency = static_cast<int>(std::max(1L, v));
                else if (a == "--per-host") o.max_per_host = static_cast<int>(std::max(1L, v));
                else if (a == "--timeout") o.timeout_sec = static_cast<int>(std::max(1L, v));
                else cli->hb_ttl = static_cast<int>(std::max(1L, v));
            }
            else if (!a.empty() && a[0] != '-' && cli->seed.empty()) {
                cli->seed = a;
            }
            else {
                return false;
            }
        }
        return cli->stats || !cli->seed.empty();
    }
}

void print_counts(const mc::TaskCounts& c) {
    std::cout << "tasks: pending=" << c.pending << " in_progress=" << c.in_progress << " done=" << c.done << " failed=" << c.failed << std::endl;
}

int main(int argc, char** argv) {
    Cli cli;
    if (!parse_args(argc, argv, &cli)) {
        usage();
        return 1;
    }
    if (cli.stats && !cli.redis)
        cli.mysql = true;                   // 单独 --stats沿用Step3的行为：看MySQL

    // ----------MySQL：页面存储（以及没有 --redis 时的任务队列）
    mc::Db db;
    std::unique_ptr<mc::MysqlFrontier> mysql_frontier;
    std::unique_ptr<mc::MysqlPageSink> sink;

    if (cli.mysql) {
        std::string err;
        if (!db.connect(mc::DbConfig::from_env(), &err)) {
            std::cerr << "mysql connect failed: " << err << std::endl;
            return 2;
        }
        if (!mc::ensure_schema(db, &err)) {
            std::cerr << "create scheme failed: " << err << std::endl;
            return 2;
        }
        mysql_frontier = std::make_unique<mc::MysqlFrontier>(db);
        sink = std::make_unique<mc::MysqlPageSink>(db);
    }

    // -------- Redis：共享任务队列-------------
    mc :: RedisClient redis_client;
    std::unique_ptr<mc::RedisFrontier> redis_frontier;
    if (cli.redis) {
        std::string err;
        if (!redis_client.connect(mc::RedisConfig::from_env(), &err)) {
            std::cerr << "redis connect failed: " << err << std::endl;
            return 2;
        }
        mc::RedisFrontierOptions ro;
        ro.ns = cli.worker_id;
        ro.worker_id = cli.worker_id;
        ro.hb_ttl_sec = cli.hb_ttl;
        redis_frontier = std::make_unique<mc::RedisFrontier>(redis_client, ro);
    }
    if (cli.stats) {
        print_counts(redis_frontier ? redis_frontier->counts() : mysql_frontier->counts());
        return 0;
    }

    auto seed = mc::parse_url(cli.seed);
    if (!seed) {
        std::cerr << "invalid seed url\n";
        return 1;
    }

    // 选定任务队列并恢复上次遗留的任务
    mc::Frontier* frontier = nullptr;
    if (redis_frontier) {
        std::string err;
        if (!redis_frontier->register_worker(&err)) {
            std::cerr << "register work failed: " << err << std::endl;
            return 2;
        }
        if (size_t n = redis_frontier->recover())
            std::cout << "recovered" << n << " interrupted task(s)\n";
        frontier = redis_frontier.get();
    }
    else if (mysql_frontier) {
        if (size_t n = mysql_frontier->recover())
            std::cout << "recovered " << n << " interrupted task(s)\n";
        frontier = mysql_frontier.get();
    }

    const auto t0 = std::chrono::steady_clock::now();
    // 注意：run()期间db只被存储线程使用，run()返回时线程已被join
    mc::AsyncCrawler crawler(cli.opt, frontier, sink.get());
    auto stats = crawler.run(*seed, [](const mc::PageResult& r){
        if (r.status == 0)
            std::cout << "[d=" << r.depth << "] ERR " << r.url << " (" << r.error << ")\n";
        else
            std::cout << "[d=" << r.depth << "] " << r.status << "  " << r.url << " (" << r.bytes << " bytes, " << r.links_found << " links)\n";
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const size_t total = stats.fetched + stats.failed;
    std::cout << "\ndone";
    if (redis_frontier)
        std::cout << " [worker " << cli.worker_id << "]";
    std::cout << "\ndone: fetched=" << stats.fetched << " failed=" << stats.failed << " skipped_non_http=" << stats.skipped_unsupported << " store_failed=" << stats.store_failed << std::endl
                << "peak_in_flight=" << stats.peak_in_flight << "  elapsed=" << secs << "s " << (secs > 0 ? total /secs : 0.0) << " pages/s\n";
    if (redis_frontier)
        print_counts(redis_frontier->counts());
    else if (mysql_frontier)
        print_counts(mysql_frontier->counts());
}