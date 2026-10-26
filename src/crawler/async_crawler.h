#pragma once
#include "crawler/crawler.h"  // CrawlOptions / CrawlStats / PageResult / PageCallback

namespace mc {

// 事件驱动的并发爬虫。on_page 回调在调用 run() 的线程里执行。
    class AsyncCrawler {
    public:
        AsyncCrawler(CrawlOptions opt, Frontier* frontier = nullptr, PageSink* sink = nullptr);
        CrawlStats run(const Url& seed, PageCallback on_page = nullptr);

    private:
        CrawlOptions opt_;
        Frontier* frontier_;
        PageSink* sink_;
    };

}  // namespace mc