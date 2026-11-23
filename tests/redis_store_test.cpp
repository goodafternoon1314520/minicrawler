#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>

#include "crawler/async_crawler.h"
#include "fake_server.h"
#include "redis/redis_client.h"
#include "redis/redis_frontier.h"
#include "redis/redis_politeness.h"

namespace {

    mc::Url U(const std::string& s) { return *mc::parse_url(s); }

    struct Node {  // 一个"爬虫进程"：自己的连接 + 自己的 frontier
        mc::RedisClient rc;
        std::unique_ptr<mc::RedisFrontier> f;
    };

    class RedisTest : public ::testing::Test {
    protected:
        void SetUp() override {
            if (!std::getenv("MC_TEST_REDIS"))
                GTEST_SKIP() << "set MC_TEST_REDIS=1 (and MC_REDIS_HOST/PORT/PASS if needed) to run Redis tests";
            static int counter = 0;
            ns = "mctest" + std::to_string(::getpid()) + "n" + std::to_string(counter++);
            std::string err;
            ASSERT_TRUE(admin.connect(mc::RedisConfig::from_env(), &err)) << err;
        }

        void TearDown() override {
            if (!admin.connected()) return;
            mc::RespValue r;
            std::string err;
            if (!admin.command({"KEYS", ns + ":*"}, &r, &err)) return;  // 只在测试库上用 KEYS
            for (const auto& k : r.array) admin.command({"DEL", k.str}, nullptr, nullptr);
        }

        std::unique_ptr<Node> node(const std::string& id, int ttl = 15, bool reg = true) {
            auto n = std::make_unique<Node>();
            std::string err;
            EXPECT_TRUE(n->rc.connect(mc::RedisConfig::from_env(), &err)) << err;
            mc::RedisFrontierOptions o;
            o.ns = ns;
            o.worker_id = id;
            o.hb_ttl_sec = ttl;
            n->f = std::make_unique<mc::RedisFrontier>(n->rc, o);
            if (reg) {
                EXPECT_TRUE(n->f->register_worker(&err)) << err;
            }
            return n;
        }

        mc::RedisClient admin;
        std::string ns;
    };

}  // namespace

TEST_F(RedisTest, PushDedupFifo) {
    auto a = node("w1", 15, false);
    auto& f = *a->f;
    EXPECT_TRUE(f.push(U("http://a.com/1"), 0));
    EXPECT_TRUE(f.push(U("http://a.com/2"), 1));
    EXPECT_FALSE(f.push(U("http://A.com/1#x"), 9));  // 规范化后重复

    auto t1 = f.pop();
    ASSERT_TRUE(t1);
    EXPECT_EQ(t1->url.to_string(), "http://a.com/1");
    EXPECT_EQ(t1->depth, 0);
    auto t2 = f.pop();
    ASSERT_TRUE(t2);
    EXPECT_EQ(t2->url.to_string(), "http://a.com/2");
    EXPECT_EQ(t2->depth, 1);
    EXPECT_FALSE(f.pop());
}

TEST_F(RedisTest, MarkSeenPreventsEnqueue) {
    auto a = node("w1", 15, false);
    a->f->mark_seen(U("http://a.com/redirected"), 1);
    EXPECT_FALSE(a->f->push(U("http://a.com/redirected"), 1));
    EXPECT_FALSE(a->f->pop());
}

TEST_F(RedisTest, CompletionBookkeeping) {
    auto a = node("w1", 15, false);
    auto& f = *a->f;
    f.push(U("http://a.com/1"), 0);
    f.push(U("http://a.com/2"), 0);
    auto t1 = f.pop();
    auto t2 = f.pop();
    ASSERT_TRUE(t1 && t2);

    auto c = f.counts();
    EXPECT_EQ(c.pending, 0u);
    EXPECT_EQ(c.in_progress, 0u + 0u);  // 未登记 worker：workers 集合里没有它，in_progress 看不到

    f.complete(t1->url, true, "");
    f.complete(t2->url, false, "boom");
    c = f.counts();
    EXPECT_EQ(c.done, 1u);
    EXPECT_EQ(c.failed, 1u);

    mc::RespValue r;
    std::string err;
    ASSERT_TRUE(a->rc.command({"HGET", ns + ":errors", "http://a.com/2"}, &r, &err)) << err;
    EXPECT_EQ(r.str, "boom");
    // 完成后 processing 列表应已清空
    ASSERT_TRUE(a->rc.command({"LLEN", ns + ":proc:w1"}, &r, &err)) << err;
    EXPECT_EQ(r.integer, 0);
}

TEST_F(RedisTest, InProgressIsCountedForRegisteredWorker) {
    auto a = node("w1");
    a->f->push(U("http://a.com/1"), 0);
    ASSERT_TRUE(a->f->pop());
    EXPECT_EQ(a->f->counts().in_progress, 1u);
}

TEST_F(RedisTest, RecoverAfterCrash) {
    auto a = node("w1", 15, false);
    a->f->push(U("http://a.com/1"), 0);
    ASSERT_TRUE(a->f->pop());  // 取走但不 complete，模拟崩溃
    EXPECT_FALSE(a->f->pop());

    auto restarted = node("w1", 15, false);  // 同一个 id "重启"
    EXPECT_EQ(restarted->f->recover(), 1u);
    auto t = restarted->f->pop();
    ASSERT_TRUE(t);
    EXPECT_EQ(t->url.to_string(), "http://a.com/1");
}

TEST_F(RedisTest, TwoWorkersShareTheQueueWithoutDuplicates) {
    auto a = node("wa");
    auto b = node("wb");
    for (int i = 0; i < 20; ++i) a->f->push(U("http://a.com/" + std::to_string(i)), 0);

    std::multiset<std::string> got;
    while (true) {
        auto t1 = a->f->pop();
        auto t2 = b->f->pop();
        if (t1) got.insert(t1->url.to_string());
        if (t2) got.insert(t2->url.to_string());
        if (!t1 && !t2) break;
    }
    EXPECT_EQ(got.size(), 20u);
    EXPECT_EQ(std::set<std::string>(got.begin(), got.end()).size(), 20u);  // 没有重复领取
}

TEST_F(RedisTest, OthersActiveFollowsTheLifecycle) {
    auto a = node("wa");
    auto b = node("wb");
    EXPECT_FALSE(b->f->others_active());  // 谁手里都没活

    a->f->push(U("http://a.com/x"), 0);
    ASSERT_TRUE(a->f->pop());
    EXPECT_TRUE(b->f->others_active());   // wa 活着且有任务在手

    a->f->complete(U("http://a.com/x"), true, "");
    EXPECT_FALSE(b->f->others_active());
}

TEST_F(RedisTest, DeadWorkersTasksAreReclaimed) {
    auto a = node("wa", /*ttl=*/1);
    a->f->push(U("http://a.com/x"), 0);
    ASSERT_TRUE(a->f->pop());
    // wa 从此不再调用任何方法 = 心跳停止 = "崩溃"

    auto b = node("wb", /*ttl=*/1);
    EXPECT_EQ(b->f->recover(), 0u);  // wa 的心跳（TTL 1 秒）还没过期，不能抢

    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // 心跳过期
    EXPECT_EQ(b->f->recover(), 1u);
    auto t = b->f->pop();
    ASSERT_TRUE(t);
    EXPECT_EQ(t->url.to_string(), "http://a.com/x");
}

// ---------- 与 AsyncCrawler 集成 ----------

namespace {

// "/" 链接到 /p/1../p/n
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

    mc::CrawlOptions fast_opts() {
        mc::CrawlOptions o;
        o.delay_ms = 0;
        o.concurrency = 8;
        o.max_per_host = 8;
        o.max_depth = 2;
        o.max_pages = 1000;
        o.timeout_sec = 5;
        o.max_retries = 0;
        return o;
    }

    mc::Url seed_for(const FakeServer& s) {
        return *mc::parse_url("http://127.0.0.1:" + std::to_string(s.port()) + "/");
    }

    // 与 async_crawler_test.cpp 里同名的辅助函数相同（两个测试文件各自独立编译，所以各放一份）
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
}  // namespace

TEST_F(RedisTest, AsyncCrawlerRunsOnRedisFrontier) {
    FakeServer srv(flat_site(20), /*delay_ms=*/20);
    auto n = node("w1");
    mc::AsyncCrawler crawler(fast_opts(), n->f.get(), nullptr);
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.fetched, 21u);
    auto c = n->f->counts();
    EXPECT_EQ(c.done, 21u);
    EXPECT_EQ(c.pending, 0u);
    EXPECT_EQ(c.in_progress, 0u);
    for (int i = 1; i <= 20; ++i) EXPECT_EQ(srv.hits("/p/" + std::to_string(i)), 1) << i;
}

TEST_F(RedisTest, TwoCrawlersShareOneQueue) {
    FakeServer srv(flat_site(40), /*delay_ms=*/50);
    auto n1 = node("w1");
    auto n2 = node("w2");
    mc::CrawlStats s1, s2;

    std::thread t1([&] {
        mc::AsyncCrawler c(fast_opts(), n1->f.get(), nullptr);
        s1 = c.run(seed_for(srv));
    });
    std::thread t2([&] {
        mc::AsyncCrawler c(fast_opts(), n2->f.get(), nullptr);
        s2 = c.run(seed_for(srv));
    });
    t1.join();
    t2.join();

    EXPECT_EQ(s1.fetched + s2.fetched, 41u);  // 所有页面恰好被抓一次，不多不少
    for (int i = 1; i <= 40; ++i) EXPECT_EQ(srv.hits("/p/" + std::to_string(i)), 1) << i;
    EXPECT_EQ(srv.hits("/"), 1);
    EXPECT_EQ(n1->f->counts().done, 41u);
}

TEST_F(RedisTest, RateLimiterIsSharedAcrossConnections) {
    mc::RedisClient c1, c2;
    std::string err;
    ASSERT_TRUE(c1.connect(mc::RedisConfig::from_env(), &err)) << err;
    ASSERT_TRUE(c2.connect(mc::RedisConfig::from_env(), &err)) << err;
    mc::RedisRateLimiter a(c1, ns), b(c2, ns);

    // 容量 3：两个"进程"合起来只能拿到 3 个令牌
    EXPECT_EQ(a.acquire("h:80", 1.0, 3.0), 0);
    EXPECT_EQ(b.acquire("h:80", 1.0, 3.0), 0);
    EXPECT_EQ(a.acquire("h:80", 1.0, 3.0), 0);
    long w = b.acquire("h:80", 1.0, 3.0);
    EXPECT_GT(w, 0);
    EXPECT_LE(w, 1000);
    EXPECT_EQ(a.acquire("other:80", 1.0, 3.0), 0);  // 别的站点是独立的桶

    mc::RespValue r;
    ASSERT_TRUE(admin.command({"PTTL", ns + ":rl:h:80"}, &r, &err)) << err;
    EXPECT_GT(r.integer, 0);    // 桶有过期时间，不会在 Redis 里无限堆积
    EXPECT_LE(r.integer, 4000);
}

TEST_F(RedisTest, RobotsStoreRoundTripAndExpiry) {
    mc::RedisClient c;
    std::string err;
    ASSERT_TRUE(c.connect(mc::RedisConfig::from_env(), &err)) << err;
    mc::RedisRobotsStore s(c, ns);

    std::string blob;
    EXPECT_FALSE(s.load("h:80", &blob));
    s.save("h:80", "Tuser-agent: *\nDisallow: /x\n", 1);  // 含换行：二进制安全
    ASSERT_TRUE(s.load("h:80", &blob));
    EXPECT_EQ(blob, "Tuser-agent: *\nDisallow: /x\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_FALSE(s.load("h:80", &blob));  // TTL 到了
}

TEST_F(RedisTest, RetryGoesThroughTheDelayedQueue) {
    auto a = node("w1");
    auto& f = *a->f;
    f.push(U("http://a.com/1"), 2);
    auto t = f.pop();
    ASSERT_TRUE(t);
    EXPECT_EQ(t->attempt, 0);

    f.retry_later(t->url, t->depth, 1, 400, "boom");

    mc::RespValue r;
    std::string err;
    ASSERT_TRUE(a->rc.command({"LLEN", ns + ":proc:w1"}, &r, &err)) << err;
    EXPECT_EQ(r.integer, 0);  // 已经从"处理中"移走
    EXPECT_EQ(f.counts().delayed, 1u);
    EXPECT_FALSE(f.pop());           // 还没到期
    EXPECT_TRUE(f.others_active());  // 有任务在等重试，不能收工

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    auto t2 = f.pop();
    ASSERT_TRUE(t2);
    EXPECT_EQ(t2->url.to_string(), "http://a.com/1");
    EXPECT_EQ(t2->depth, 2);
    EXPECT_EQ(t2->attempt, 1);
    EXPECT_EQ(f.counts().delayed, 0u);
    EXPECT_FALSE(f.others_active());
}

TEST_F(RedisTest, OldQueueItemFormatStillParses) {
    auto a = node("w1", 15, false);
    mc::RespValue r;
    std::string err;
    ASSERT_TRUE(a->rc.command({"LPUSH", ns + ":pending", std::string("3 http://a.com/x")}, &r, &err)) << err;
    auto t = a->f->pop();
    ASSERT_TRUE(t);
    EXPECT_EQ(t->depth, 3);
    EXPECT_EQ(t->attempt, 0);
}

TEST_F(RedisTest, AsyncCrawlerRetriesThroughTheRedisDelayedQueue) {
    FakeServer srv(flaky(flat_site(10), 1));
    auto n = node("w1");
    auto opt = fast_opts();
    opt.max_retries = 3;
    opt.retry_base_ms = 30;
    opt.retry_max_ms = 300;
    mc::AsyncCrawler crawler(opt, n->f.get(), nullptr);
    auto stats = crawler.run(seed_for(srv));

    EXPECT_EQ(stats.fetched, 11u);
    EXPECT_EQ(stats.failed, 0u);
    EXPECT_EQ(stats.retried, 10u);
    auto c = n->f->counts();
    EXPECT_EQ(c.done, 11u);
    EXPECT_EQ(c.pending, 0u);
    EXPECT_EQ(c.in_progress, 0u);
    EXPECT_EQ(c.delayed, 0u);
}