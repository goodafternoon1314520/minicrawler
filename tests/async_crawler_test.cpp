#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "crawler/async_crawler.h"
#include "fake_server.h"

namespace {

// 写入发生在存储线程；run() 返回前该线程已被 join，所以之后读取是安全的
    struct CollectSink : mc::PageSink {
        std::vector<mc::PageRecord> saved;
        bool save(const mc::PageRecord& p, std::string*) override {
            saved.push_back(p);
            return true;
        }
    };

// "/" 链接到 /p/1../p/n；/p/i 只有一行文字
    FakeServer::Handler flat_site(int n) {
        return [n](const std::string& path, std::string* body) {
            if (path == "/") {
                *body = "<html><body>";
                for (int i = 1; i <= n; ++i) *body += "<a href=\"/p/" + std::to_string(i) + "\">x</a>";
                *body += "</body></html>";
                return true;
            }
            if (path.rfind("/p/", 0) == 0) {
                *body = "<html>page " + path.substr(3) + "</html>";
                return true;
            }
            return false;
        };
    }

    mc::Url seed_for(const FakeServer& s) {
        return *mc::parse_url("http://127.0.0.1:" + std::to_string(s.port()) + "/");
    }

    mc::CrawlOptions fast_opts() {
        mc::CrawlOptions o;
        o.delay_ms = 0;
        o.concurrency = 10;
        o.max_per_host = 10;
        o.max_depth = 2;
        o.max_pages = 1000;
        o.timeout_sec = 5;
        return o;
    }

}  // namespace

TEST(AsyncCrawlerTest, CrawlsWholeSiteConcurrently) {
    const int N = 30;
    FakeServer srv(flat_site(N), /*delay_ms=*/100);
    CollectSink sink;
    mc::AsyncCrawler crawler(fast_opts(), nullptr, &sink);

    const auto t0 = std::chrono::steady_clock::now();
    auto stats = crawler.run(seed_for(srv));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ(stats.fetched, static_cast<size_t>(N + 1));
    EXPECT_EQ(stats.failed, 0u);
    ASSERT_EQ(sink.saved.size(), static_cast<size_t>(N + 1));
    EXPECT_EQ(srv.hits("/"), 1);
    for (int i = 1; i <= N; ++i) EXPECT_EQ(srv.hits("/p/" + std::to_string(i)), 1) << i;

    EXPECT_GE(stats.peak_in_flight, 5u);
    EXPECT_GE(srv.max_active(), 5);
    EXPECT_LT(ms, 2000);  // 串行需要 31 * 100ms = 3100ms
}

TEST(AsyncCrawlerTest, MaxPagesIsExact) {
    FakeServer srv(flat_site(30));
    auto opt = fast_opts();
    opt.max_pages = 5;
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(srv.total(), 5);
    EXPECT_EQ(stats.fetched, 5u);
}

TEST(AsyncCrawlerTest, PerHostLimitIsRespected) {
    FakeServer srv(flat_site(20), /*delay_ms=*/50);
    auto opt = fast_opts();
    opt.concurrency = 10;
    opt.max_per_host = 2;  // 全局允许 10，但同一站点最多 2 个
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(stats.fetched, 21u);
    EXPECT_LE(srv.max_active(), 2);
    EXPECT_GE(srv.max_active(), 2);
}

TEST(AsyncCrawlerTest, DedupMissingAndDeadLinks) {
    FakeServer srv([](const std::string& path, std::string* body) {
        if (path == "/") {
            *body = R"(<a href="/a">a</a><a href="/a#frag">a2</a><a href="/missing">m</a>)"
                    R"(<a href="http://127.0.0.1:1/dead">d</a>)";
            return true;
        }
        if (path == "/a") {
            *body = R"(<a href="/">home</a><a href="/missing">m</a>)";
            return true;
        }
        return false;  // 其余 404
    });
    mc::AsyncCrawler crawler(fast_opts());
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(srv.hits("/"), 1);
    EXPECT_EQ(srv.hits("/a"), 1);        // "/a" 与 "/a#frag" 去重
    EXPECT_EQ(srv.hits("/missing"), 1);  // 两个页面都链接它，只抓一次
    EXPECT_EQ(stats.fetched, 3u);        // 404 也是一次成功的 HTTP 交互
    EXPECT_EQ(stats.failed, 1u);         // 127.0.0.1:1 连接被拒
}

TEST(AsyncCrawlerTest, TerminatesWhenNothingToDo) {
    FakeServer srv(flat_site(0));  // 只有首页，没有任何链接
    mc::AsyncCrawler crawler(fast_opts());
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(stats.fetched, 1u);
}