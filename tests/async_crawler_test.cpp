#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

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
        o.obey_robots = false;                  // 旧测试不关心robots，需要的测试自己打开
        o.max_retries = 0;                      // 旧测试不关心重试，需要的测试自己打开
        return o;
    }

    // 在任意站点之上叠加一个 /robots.txt；robots 为空串表示"没有这个文件"（404）
    FakeServer::Handler with_robots(FakeServer::Handler inner, std::string robots) {
        return [inner, robots](const std::string& path, std::string* body) {
            if (path == "/robots.txt") {
                if (robots.empty()) return false;
                *body = robots;
                return true;
            }
            return inner(path, body);
        };
    }

    mc::CrawlOptions polite_opts() {
        auto o = fast_opts();
        o.obey_robots = true;
        return o;
    }

    long ms_since(std::chrono::steady_clock::time_point t0) {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }

    // 每个 /p/ 页面的前 fail_times 次请求返回 fail_body，之后正常。handler 会被多个连接线程同时调用，所以要加锁。
    FakeServer::Handler flaky(FakeServer::Handler inner, int fail_times, std::string fail_body = "@status:503") {
        auto mu = std::make_shared<std::mutex>();
        auto counts = std::make_shared<std::map<std::string, int>>();
        return [=](const std::string& path, std::string* body) {
            if (path.rfind("/p/", 0) == 0) {
                std::lock_guard<std::mutex> lk(*mu);
                if ((*counts)[path]++ < fail_times) {
                    *body = fail_body;
                    return true;
                }
            }
            return inner(path, body);
        };
    }

    mc::CrawlOptions retry_opts() {
        auto o = fast_opts();
        o.max_retries = 3;
        o.retry_base_ms = 20;
        o.retry_max_ms = 500;
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

TEST(AsyncCrawlerPolicyTest, RobotsDisallowAndAllowAreObeyed) {
    // /p/1 与 /p/11../p/19 被前缀规则挡住；/p/10 被更长的 Allow 规则放行
    FakeServer srv(with_robots(flat_site(25), "User-agent: *\nDisallow: /p/1\nAllow: /p/10$\n"));
    mc::AsyncCrawler crawler(polite_opts());
    // auto stats = crawler.run(seed_for(srv));
    auto stats = crawler.run(seed_for(srv), [](const mc::PageResult& r) {
        std::cerr << "status=" << r.status << " links=" << r.links_found << " skipped=" << r.skipped << " " << r.url << std::endl;
    });

    EXPECT_EQ(stats.skipped_robots, 10u);
    EXPECT_EQ(stats.fetched, 16u);  // "/" + 25 个页面 - 10 个被禁止的
    EXPECT_EQ(stats.robots_fetched, 1u);
    EXPECT_EQ(srv.hits("/robots.txt"), 1);  // 这么多页面，robots.txt 只抓一次
    EXPECT_EQ(srv.hits("/p/1"), 0);
    EXPECT_EQ(srv.hits("/p/15"), 0);
    EXPECT_EQ(srv.hits("/p/10"), 1);
    EXPECT_EQ(srv.hits("/p/2"), 1);
}

TEST(AsyncCrawlerPolicyTest, MissingRobotsMeansEverythingIsAllowed) {
    FakeServer srv(with_robots(flat_site(10), ""));  // /robots.txt -> 404
    mc::AsyncCrawler crawler(polite_opts());
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(stats.fetched, 11u);
    EXPECT_EQ(stats.skipped_robots, 0u);
    EXPECT_EQ(srv.hits("/robots.txt"), 1);
}

TEST(AsyncCrawlerPolicyTest, ObeyRobotsCanBeSwitchedOff) {
    FakeServer srv(with_robots(flat_site(5), "User-agent: *\nDisallow: /\n"));
    auto opt = fast_opts();  // obey_robots = false
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(stats.fetched, 6u);
    EXPECT_EQ(srv.hits("/robots.txt"), 0);
}

TEST(AsyncCrawlerPolicyTest, DisallowAllStopsBeforeTheSeed) {
    FakeServer srv(with_robots(flat_site(5), "User-agent: *\nDisallow: /\n"));
    mc::AsyncCrawler crawler(polite_opts());
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(stats.fetched, 0u);
    EXPECT_EQ(stats.skipped_robots, 1u);
    EXPECT_EQ(srv.hits("/"), 0);
}

TEST(AsyncCrawlerPolicyTest, RateLimitSpacesRequests) {
    FakeServer srv(flat_site(20));
    auto opt = fast_opts();
    opt.host_rate = 20;  // 每秒 20 个 = 每 50ms 一个
    opt.host_burst = 1;
    mc::AsyncCrawler crawler(opt);

    const auto t0 = std::chrono::steady_clock::now();
    auto stats = crawler.run(seed_for(srv));
    const long ms = ms_since(t0);

    EXPECT_EQ(stats.fetched, 21u);
    EXPECT_GE(ms, 800);   // 理想值约 1000ms；没有限速时只要几十毫秒
    EXPECT_LT(ms, 4000);
}

TEST(AsyncCrawlerPolicyTest, CrawlDelayInRobotsSlowsTheCrawl) {
    FakeServer srv(with_robots(flat_site(10), "User-agent: *\nCrawl-delay: 0.1\n"));  // 等价于每秒 10 个
    mc::AsyncCrawler crawler(polite_opts());

    const auto t0 = std::chrono::steady_clock::now();
    auto stats = crawler.run(seed_for(srv));
    const long ms = ms_since(t0);

    EXPECT_EQ(stats.fetched, 11u);
    EXPECT_GE(ms, 800);
    EXPECT_LT(ms, 4000);
}

TEST(AsyncCrawlerPolicyTest, CrawlDelayIsCappedByMaxCrawlDelay) {
    FakeServer srv(with_robots(flat_site(10), "User-agent: *\nCrawl-delay: 3600\n"));
    auto opt = polite_opts();
    opt.max_crawl_delay = 0.05;  // 上限压到 50ms
    mc::AsyncCrawler crawler(opt);

    const auto t0 = std::chrono::steady_clock::now();
    auto stats = crawler.run(seed_for(srv));
    EXPECT_EQ(stats.fetched, 11u);
    EXPECT_LT(ms_since(t0), 3000);  // 否则要等几个小时
}

TEST(AsyncCrawlerPolicyTest, RedirectTargetsGoThroughTheFrontier) {
    FakeServer srv([](const std::string& p, std::string* b) {
        if (p == "/") { *b = R"(<a href="/old">o</a>)"; return true; }
        if (p == "/old") { *b = "@redirect:/new"; return true; }
        if (p == "/new") { *b = R"(<a href="/deep">d</a>)"; return true; }
        if (p == "/deep") { *b = ""; return true; }
        return false;
    });
    CollectSink sink;
    mc::AsyncCrawler crawler(fast_opts(), nullptr, &sink);
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.fetched, 4u);        // "/"、"/old"(301)、"/new"、"/deep"
    EXPECT_EQ(sink.saved.size(), 3u);    // 301 本身不保存
    EXPECT_EQ(srv.hits("/deep"), 1);     // 跳转不算一层深度，所以 /deep 仍在 max_depth=2 之内
}

TEST(AsyncCrawlerPolicyTest, RedirectIntoDisallowedPathIsBlocked) {
    FakeServer srv(with_robots(
            [](const std::string& p, std::string* b) {
                if (p == "/") { *b = R"(<a href="/old">o</a>)"; return true; }
                if (p == "/old") { *b = "@redirect:/secret"; return true; }
                if (p == "/secret") { *b = "classified"; return true; }
                return false;
            },
            "User-agent: *\nDisallow: /secret\n"));
    mc::AsyncCrawler crawler(polite_opts());
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(srv.hits("/secret"), 0);  // 绕不过去
    EXPECT_EQ(stats.skipped_robots, 1u);
    EXPECT_EQ(stats.fetched, 2u);
}

TEST(AsyncCrawlerRetryTest, TransientFailuresAreRetriedUntilSuccess) {
    FakeServer srv(flaky(flat_site(5), 2));
    mc::AsyncCrawler crawler(retry_opts());
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.fetched, 6u);
    EXPECT_EQ(stats.failed, 0u);
    EXPECT_EQ(stats.retried, 10u);  // 5 个页面 x 失败 2 次
    for (int i = 1; i <= 5; ++i) EXPECT_EQ(srv.hits("/p/" + std::to_string(i)), 3) << i;
    EXPECT_EQ(stats.by_status[503], 10u);
    EXPECT_EQ(stats.by_status[200], 6u);
}

TEST(AsyncCrawlerRetryTest, GivesUpAfterMaxRetries) {
    FakeServer srv(flaky(flat_site(5), 1000));
    auto opt = retry_opts();
    opt.max_retries = 2;
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.fetched, 1u);  // 只有 "/" 成功
    EXPECT_EQ(stats.failed, 5u);
    EXPECT_EQ(stats.retried, 10u);
    EXPECT_EQ(srv.hits("/p/1"), 3);  // 1 次原始 + 2 次重试
}

TEST(AsyncCrawlerRetryTest, PermanentFailuresAreNotRetried) {
    FakeServer srv([](const std::string& p, std::string* b) {
        if (p == "/") {
            *b = R"(<a href="/gone">g</a>)";
            return true;
        }
        return false;  // 404
    });
    mc::AsyncCrawler crawler(retry_opts());
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(srv.hits("/gone"), 1);
    EXPECT_EQ(stats.retried, 0u);
    EXPECT_EQ(stats.failed, 0u);   // 404 是一次成功的 HTTP 交互
    EXPECT_EQ(stats.fetched, 2u);
}

TEST(AsyncCrawlerRetryTest, ConnectionRefusedIsRetriedThenGivenUp) {
    FakeServer srv([](const std::string&, std::string* b) {
        *b = R"(<a href="http://127.0.0.1:1/dead">d</a>)";
        return true;
    });
    auto opt = retry_opts();
    opt.max_retries = 1;
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.fetched, 1u);
    EXPECT_EQ(stats.retried, 1u);
    EXPECT_EQ(stats.failed, 1u);
}

TEST(AsyncCrawlerRetryTest, RetryAfterIsHonored) {
    FakeServer srv(flaky(flat_site(1), 1, "@status:429:1"));
    auto opt = retry_opts();
    opt.retry_max_ms = 5000;  // 上限必须大于 Retry-After 的 1 秒，否则会被判定为"等太久"而放弃
    const auto t0 = std::chrono::steady_clock::now();
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));
    const long ms = ms_since(t0);

    EXPECT_EQ(stats.retried, 1u);
    EXPECT_EQ(stats.fetched, 2u);
    EXPECT_GE(ms, 900);
    EXPECT_LT(ms, 4000);
}

TEST(AsyncCrawlerRetryTest, RetryAfterBeyondTheCapGivesUpImmediately) {
    FakeServer srv(flaky(flat_site(1), 1000, "@status:503:3600"));  // 让我们等一小时
    auto opt = retry_opts();
    opt.retry_max_ms = 500;
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.retried, 0u);
    EXPECT_EQ(stats.failed, 1u);
    EXPECT_EQ(srv.hits("/p/1"), 1);
}

TEST(AsyncCrawlerRetryTest, BackoffGrowsExponentially) {
    FakeServer srv(flaky(flat_site(1), 3));
    auto opt = retry_opts();
    opt.retry_base_ms = 100;
    opt.retry_max_ms = 10000;
    const auto t0 = std::chrono::steady_clock::now();
    mc::AsyncCrawler crawler(opt);
    auto stats = crawler.run(seed_for(srv));
    const long ms = ms_since(t0);

    EXPECT_EQ(stats.retried, 3u);
    EXPECT_EQ(srv.hits("/p/1"), 4);
    EXPECT_GE(ms, 300);   // 100+200+400，抖动最少打五折也有 350ms
    EXPECT_LT(ms, 3000);
}

TEST(AsyncCrawlerMetricsTest, LatencyBytesAndStatusCounts) {
    FakeServer srv(flat_site(10), /*delay_ms=*/20);
    mc::AsyncCrawler crawler(fast_opts());
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.by_status[200], 11u);
    EXPECT_GT(stats.bytes, 0u);
    EXPECT_GE(stats.latency_p50_ms, 15.0);
    EXPECT_LE(stats.latency_p50_ms, stats.latency_p95_ms);
    EXPECT_LE(stats.latency_p95_ms, stats.latency_max_ms);
}