#pragma once
#include <functional>
#include <optional>
#include <string>

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
    };

    struct PageResult {
        std::string url;
        int depth = 0;
        int status = 0;  // 0 表示请求失败
        size_t bytes = 0;
        size_t links_found = 0;
        std::string error;
    };

    struct CrawlStats {
        size_t fetched = 0;
        size_t failed = 0;
        size_t skipped_unsupported = 0;
        size_t store_failed = 0;  // 入库失败的页面数
        size_t peak_in_flight = 0;          // 运行期间同时在途的请求峰值
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