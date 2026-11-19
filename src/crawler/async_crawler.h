#pragma once
#include "crawler/crawler.h"  // CrawlOptions / CrawlStats / PageResult / PageCallback
#include "crawler/politeness.h"

namespace mc {

// 事件驱动的并发爬虫。on_page 回调在调用 run() 的线程里执行。
    class AsyncCrawler {
    public:
        // frontier / sink / pol 里的对象都不归 AsyncCrawler 所有。
        // pol 里的字段为空时使用进程内的内存实现（单进程场景）。
        AsyncCrawler(CrawlOptions opt, Frontier* frontier = nullptr, PageSink* sink = nullptr,
                     Politeness pol = {});
        CrawlStats run(const Url& seed, PageCallback on_page = nullptr);

    private:
        CrawlOptions opt_;
        Frontier* frontier_;
        PageSink* sink_;
        Politeness pol_;
    };

}  // namespace mc