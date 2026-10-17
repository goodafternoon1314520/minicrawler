#pragma once

#include <functional>
#include <optional>
#include <string>

#include "../net/http_response.h"
#include "../util/url.h"

namespace mc {
    struct CrawlOptions {
        int max_depth = 2;      // 种子深度为0
        size_t max_pages = 50;  // 最多尝试抓取多少各网页（含失败）
        bool same_host_only = true;     // 只抓和种子同一个host的网页
        int delay_ms = 500;             // 两次请求问题
        int max_redirects = 3;
    };

    struct PageResult {
        std::string url;        // 最终URL
        int depth = 0;
        int status = 0;         // 0表示请求失败
        size_t bytes = 0;
        size_t links_found = 0;
        std::string error;
    };

    struct CrawlStats {
        size_t fetched = 0;     // 拿到了HTTP响应（不论状态码）
        size_t failed = 0;      // 连接/超时/解析失败
        size_t skipped_unsupported = 0;     // 暂不支持连接，https
    };

// 抓取函数可以替换，生产用真实http_get，测试用假网页
    using Fetcher = std::function<std::optional<HttpResponse>(const Url &, std::string *)>;
    using PageCallback = std::function<void(const PageResult&)>;

    Fetcher default_fetcher(int timeout_sec = 10);

    class Crawler {
    public:
        Crawler(CrawlOptions opt, Fetcher fetcher);
        CrawlStats run(const Url& seed, PageCallback on_page = nullptr);

    private:
        CrawlOptions opt_;
        Fetcher fetch_;
    };
}