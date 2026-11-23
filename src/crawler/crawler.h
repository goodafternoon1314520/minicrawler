#pragma once
#include <functional>
#include <optional>
#include <string>
#include <map>
#include <cstdint>

#include "crawler/frontier.h"
#include "crawler/page_sink.h"
#include "net/http_response.h"
#include "util/url.h"

namespace mc {

    struct CrawlOptions {
        int max_depth = 2;           // 种子深度为 0
        size_t max_pages = 50;       // 本次运行最多尝试抓取多少个页面（含失败）
        bool same_host_only = true;
        int delay_ms = 500;
        int max_redirects = 3;
        int concurrency = 16;        // 同时进行的请求数
        int max_per_host = 8;       // 同一站点同时进行的请求上限
        int timeout_sec = 10;

        bool obey_robots = true;        // 抓取前先读robots.txt（只有AsyncCrawler实现了他）
        double host_rate = 0;           // 每个站点每秒最多多少个请求（所有进程合计）：0 = 不启用
        double host_burst = 1;          // 令牌桶容量，允许瞬间连发几个
        double max_crawl_delay = 30;    // robots.txt 的Crawl-delay最多尊重到多少秒

        int max_retries = 3;            // 一个任务最多重试几次（0 = 不重试）
        int retry_base_ms = 1000;       // 第1次重试前的基准等待，之后每次翻倍
        int retry_max_ms = 60000;       // 单次等待上限，服务器要求等的更久（Retry-After）就直接放弃
        int progress_sec = 0;           // >0时每隔这么多秒向stderr打印一行进度
    };

    struct PageResult {
        std::string url;
        int depth = 0;
        int status = 0;  // 0 表示请求失败
        size_t bytes = 0;
        size_t links_found = 0;
        std::string error;
        bool skipped = false;           // 被robots.txt拒绝，没有真正发出请求

        int attempt = 0;                // 这是第几次尝试（0 = 第一次）
        int retrying = false;           // 这次失败，已安排重试
        long retry_in_ms = 0;           // 多次之后重试
    };

    struct CrawlStats {
        size_t fetched = 0;
        size_t failed = 0;
        size_t skipped_unsupported = 0;
        size_t store_failed = 0;  // 入库失败的页面数
        size_t peak_in_flight = 0;          // 运行期间同时在途的请求峰值
        size_t skipped_robots = 0;          // 被robots.txt拒绝的任务数
        size_t robots_fetched = 0;          // 实际发出的robots.txt请求数（命中缓存的不算）

        size_t retried = 0;                 // 被安排重试的次数（不是任务数）
        uint64_t bytes = 0;                 // 成功响应的body字节总数
        std::map<int, size_t> by_status;    // 每次尝试的结果，状态码 -> 次数，0 = 没拿到响应
        double latency_p50_ms = 0, latency_p95_ms = 0, latency_max_ms = 0;  // 每次尝试从发起到收完
    };

    using Fetcher = std::function<std::optional<HttpResponse>(const Url&, std::string*)>;
    using PageCallback = std::function<void(const PageResult&)>;

    Fetcher default_fetcher(int timeout_sec = 10);

    class Crawler {
    public:
        // frontier / sink 都不归 Crawler 所有，生命周期由调用方保证。
        // frontier 为空 -> 用内存队列；sink 为空 -> 不保存页面。
        Crawler(CrawlOptions opt, Fetcher fetcher, Frontier* frontier = nullptr,
                PageSink* sink = nullptr);
        CrawlStats run(const Url& seed, PageCallback on_page = nullptr);

    private:
        CrawlOptions opt_;
        Fetcher fetch_;
        Frontier* frontier_;
        PageSink* sink_;
    };

}  // namespace mc