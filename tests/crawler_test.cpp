#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "crawler/crawler.h"

namespace {

    struct FakeWeb {
        std::map<std::string, mc::HttpResponse> pages;
        std::vector<std::string> fetched;  // 记录真正被请求的 URL 顺序

        void add(const std::string& url, const std::string& body,
                 const std::string& ctype = "text/html") {
            mc::HttpResponse r;
            r.status = 200;
            r.headers["content-type"] = ctype;
            r.body = body;
            pages[url] = r;
        }
        void redirect(const std::string& url, const std::string& to) {
            mc::HttpResponse r;
            r.status = 301;
            r.headers["location"] = to;
            pages[url] = r;
        }
        mc::Fetcher fetcher() {
            return [this](const mc::Url& u, std::string* err) -> std::optional<mc::HttpResponse> {
                fetched.push_back(u.to_string());
                auto it = pages.find(u.to_string());
                if (it == pages.end()) {
                    if (err) *err = "no such page";
                    return std::nullopt;
                }
                return it->second;
            };
        }
    };

    mc::CrawlOptions fast() {
        mc::CrawlOptions o;
        o.delay_ms = 0;
        return o;
    }

    mc::Url seed() { return *mc::parse_url("http://t.com/"); }

}  // namespace

TEST(CrawlerTest, BfsOrderDedupAndDepthLimit) {
    FakeWeb web;
    web.add("http://t.com/", R"(<a href="/b">b</a><a href="/c">c</a>)");
    web.add("http://t.com/b", R"(<a href="/">home</a><a href="/d">d</a>)");
    web.add("http://t.com/c", R"(<a href="/d#frag">d</a>)");  // fragment 不应造成重复
    web.add("http://t.com/d", R"(<a href="/e">e</a>)");
    web.add("http://t.com/e", "");

    auto opt = fast();
    opt.max_depth = 2;
    mc::Crawler c(opt, web.fetcher());

    std::vector<int> depths;
    auto stats = c.run(seed(), [&](const mc::PageResult& r) { depths.push_back(r.depth); });

    // BFS 顺序；d 在深度 2 不再展开，所以 e 不会被抓；每页只抓一次
    EXPECT_EQ(web.fetched, (std::vector<std::string>{"http://t.com/", "http://t.com/b",
                                                     "http://t.com/c", "http://t.com/d"}));
    EXPECT_EQ(depths, (std::vector<int>{0, 1, 1, 2}));
    EXPECT_EQ(stats.fetched, 4u);
    EXPECT_EQ(stats.failed, 0u);
}

TEST(CrawlerTest, SameHostOnly) {
    FakeWeb web;
    web.add("http://t.com/", R"(<a href="http://other.com/x">o</a><a href="/b">b</a>)");
    web.add("http://t.com/b", "");

    mc::Crawler c1(fast(), web.fetcher());
    c1.run(seed());
    EXPECT_EQ(web.fetched, (std::vector<std::string>{"http://t.com/", "http://t.com/b"}));

    web.fetched.clear();
    auto opt = fast();
    opt.same_host_only = false;
    mc::Crawler c2(opt, web.fetcher());
    auto stats = c2.run(seed());
    EXPECT_EQ(web.fetched.size(), 3u);  // other.com/x 被尝试了
    EXPECT_EQ(stats.failed, 1u);        // 假网页里没有它
}

TEST(CrawlerTest, MaxPages) {
    FakeWeb web;
    std::string root;
    for (int i = 1; i <= 5; ++i) {
        root += "<a href=\"/" + std::to_string(i) + "\">x</a>";
        web.add("http://t.com/" + std::to_string(i), "");
    }
    web.add("http://t.com/", root);

    auto opt = fast();
    opt.max_pages = 3;
    mc::Crawler c(opt, web.fetcher());
    c.run(seed());
    EXPECT_EQ(web.fetched.size(), 3u);
}

TEST(CrawlerTest, RedirectResolvesLinksAgainstFinalUrl) {
    FakeWeb web;
    web.add("http://t.com/", R"(<a href="/old">old</a>)");
    web.redirect("http://t.com/old", "/new/dir/");
    web.add("http://t.com/new/dir/", R"(<a href="page2">p2</a>)");  // 相对链接
    web.add("http://t.com/new/dir/page2", "");

    mc::Crawler c(fast(), web.fetcher());
    c.run(seed());
    EXPECT_EQ(web.fetched,
              (std::vector<std::string>{"http://t.com/", "http://t.com/old",
                                        "http://t.com/new/dir/",
                                        "http://t.com/new/dir/page2"}));
}

TEST(CrawlerTest, NonHtmlIsNotParsed) {
    FakeWeb web;
    web.add("http://t.com/", R"(<a href="/img">i</a>)");
    web.add("http://t.com/img", R"(<a href="/trap">fake</a>)", "image/png");
    web.add("http://t.com/trap", "");

    mc::Crawler c(fast(), web.fetcher());
    c.run(seed());
    EXPECT_EQ(web.fetched, (std::vector<std::string>{"http://t.com/", "http://t.com/img"}));
}

TEST(CrawlerTest, HttpsLinksAreSkippedAndCounted) {
    FakeWeb web;
    web.add("http://t.com/", R"(<a href="https://t.com/s">s</a><a href="mailto:a@b.c">m</a>)");

    mc::Crawler c(fast(), web.fetcher());
    auto stats = c.run(seed());
    EXPECT_EQ(web.fetched.size(), 1u);
    EXPECT_EQ(stats.skipped_unsupported, 1u);  // mailto 在 resolve 阶段就被丢掉，不计入
}