//
// Created by davidcopperfield on 26-10-15.
//
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <string>
#include <memory>

#include "crawler/async_crawler.h"
#include "storage/mysql_store.h"
#include "storage/mysql_db.h"
#include "util/url.h"

namespace {
    struct Cli {
        std::string seed;
        bool mysql = false;
        bool stats = false;
        mc::CrawlOptions opt;
    };

    void usage() {
        std::cerr << "Usage: minicrawler <seed_url> [--depth N] [--max-pages N] [--delay-ms N]\n"
                     "                              [--any-host] [--mysql]\n"
                     "MySQL connection comes from env: MC_DB_HOST MC_DB_PORT MC_DB_USER MC_DB_PASS MC_DB_NAME\n";
    }

    bool to_long(const char *s, long *out) {
        char *end = nullptr;
        long v = std::strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0')
            return false;
        *out = v;
        return true;
    }

    bool parse_args(int argc, char** argv, Cli* cli) {
        auto& o = cli->opt;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            long v = 0;
            if (a == "--any-host") {
                o.same_host_only = false;
            } else if (a == "--mysql") {
                cli->mysql = true;
            } else if (a == "--stats") {
                cli->stats = cli->mysql = true;
            } else if ((a == "--depth" || a == "--max-pages" || a == "--delay-ms" ||
                        a == "--concurrency" || a == "--per-host" || a == "--timeout") &&
                       i + 1 < argc && to_long(argv[i + 1], &v) && v >= 0) {
                ++i;
                if (a == "--depth") o.max_depth = static_cast<int>(v);
                else if (a == "--max-pages") o.max_pages = static_cast<size_t>(v);
                else if (a == "--delay-ms") o.delay_ms = static_cast<int>(v);
                else if (a == "--concurrency") o.concurrency = static_cast<int>(std::max(1L, v));
                else if (a == "--per-host") o.max_per_host = static_cast<int>(std::max(1L, v));
                else o.timeout_sec = static_cast<int>(std::max(1L, v));
            } else if (!a.empty() && a[0] != '-' && cli->seed.empty()) {
                cli->seed = a;
            } else {
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

    mc::Db db;
    std::unique_ptr<mc::MysqlFrontier> frontier;
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
        frontier = std::make_unique<mc::MysqlFrontier>(db);
        sink = std::make_unique<mc::MysqlPageSink>(db);

        if (cli.stats) {
            print_counts(frontier->counts());
            return 0;
        }
        // 上次如果崩溃推出的，有任务会卡在in_progress，这里把他们放回队列
        if (size_t n = frontier->recover())
            std::cout << "recovered " << n << " interrupted task(s)\n";
    }

    auto seed = mc::parse_url(cli.seed);
    if (!seed) {
        std::cerr << "invalid seed url\n";
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    // 注意：run()期间db只被存储线程使用，run()返回时线程已被join
    mc::Crawler crawler(cli.opt, mc::default_fetcher(), frontier.get(), sink.get());
    auto stats = crawler.run(*seed, [](const mc::PageResult& r){
        if (r.status == 0)
            std::cout << "[d=" << r.depth << "] ERR " << r.url << " (" << r.error << ")\n";
        else
            std::cout << "[d=" << r.depth << "] " << r.status << "  " << r.url << " (" << r.bytes << " bytes, " << r.links_found << " links)\n";
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const size_t total = stats.fetched + stats.failed;
    std::cout << "\ndone: fetched=" << stats.fetched << " failed=" << stats.failed << " skipped_non_http=" << stats.skipped_unsupported << " store_failed=" << stats.store_failed << std::endl
                << "peak_in_flight=" << stats.peak_in_flight << "  elapsed=" << secs << "s " << (secs > 0 ? total /secs : 0.0) << " pages/s\n";
    if (frontier)
        print_counts(frontier->counts());
}