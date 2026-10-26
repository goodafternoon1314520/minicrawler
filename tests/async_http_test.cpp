#include <gtest/gtest.h>

#include <chrono>

#include "fake_server.h"
#include "net/async_http.h"
#include "net/event_loop.h"

namespace {

    using Clock = std::chrono::steady_clock;

    struct Result {
        bool done = false;
        std::optional<mc::HttpResponse> resp;
        std::string err;
    };

    mc::Url url_for(int port, const std::string& path) {
        mc::Url u;
        u.scheme = "http";
        u.host = "127.0.0.1";
        u.port = port;
        u.path = path;
        return u;
    }

// 驱动事件循环，直到 done() 为真或超时
    template <typename Done>
    bool run_until(mc::EventLoop& loop, mc::AsyncHttpClient& client, Done done, int limit_ms = 5000) {
        const auto end = Clock::now() + std::chrono::milliseconds(limit_ms);
        while (!done()) {
            if (Clock::now() > end) return false;
            client.tick();
            loop.poll(20);
        }
        return true;
    }

    auto echo_handler() {
        return [](const std::string& p, std::string* body) {
            *body = "hello " + p;
            return true;
        };
    }

}  // namespace

TEST(AsyncHttpTest, SingleGet) {
    FakeServer srv(echo_handler());
    mc::EventLoop loop;
    mc::AsyncHttpClient client(loop, 3);

    Result r;
    client.get(url_for(srv.port(), "/x"), [&](std::optional<mc::HttpResponse> resp, const std::string& err) {
        r.done = true;
        r.resp = std::move(resp);
        r.err = err;
    });
    EXPECT_FALSE(r.done);  // 回调绝不会在 get() 返回之前执行
    ASSERT_TRUE(run_until(loop, client, [&] { return r.done; }));
    ASSERT_TRUE(r.resp) << r.err;
    EXPECT_EQ(r.resp->status, 200);
    EXPECT_EQ(r.resp->body, "hello /x");
    EXPECT_EQ(client.active(), 0u);
}

TEST(AsyncHttpTest, ManyConcurrentRequestsOnOneThread) {
    FakeServer srv(echo_handler(), /*delay_ms=*/100);
    mc::EventLoop loop;
    mc::AsyncHttpClient client(loop, 5);

    const int N = 30;
    std::vector<Result> rs(N);
    int finished = 0;
    for (int i = 0; i < N; ++i) {
        client.get(url_for(srv.port(), "/" + std::to_string(i)),
                   [&, i](std::optional<mc::HttpResponse> resp, const std::string& err) {
                       rs[i].resp = std::move(resp);
                       rs[i].err = err;
                       ++finished;
                   });
    }
    const auto t0 = Clock::now();
    ASSERT_TRUE(run_until(loop, client, [&] { return finished == N; }));
    const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(rs[i].resp) << i << ": " << rs[i].err;
        EXPECT_EQ(rs[i].resp->body, "hello /" + std::to_string(i));
    }
    EXPECT_GE(srv.max_active(), 10);  // 服务端确实看到了大量同时在途的请求
    EXPECT_LT(ms, 2000);              // 串行至少要 30 * 100ms = 3000ms
}

TEST(AsyncHttpTest, TimeoutOnSilentServer) {
    FakeServer srv(echo_handler(), 0, /*hang=*/true);
    mc::EventLoop loop;
    mc::AsyncHttpClient client(loop, /*timeout_sec=*/1);

    Result r;
    client.get(url_for(srv.port(), "/"), [&](std::optional<mc::HttpResponse> resp, const std::string& err) {
        r.done = true;
        r.resp = std::move(resp);
        r.err = err;
    });
    const auto t0 = Clock::now();
    ASSERT_TRUE(run_until(loop, client, [&] { return r.done; }));
    const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();

    EXPECT_FALSE(r.resp);
    EXPECT_NE(r.err.find("timeout"), std::string::npos) << r.err;
    EXPECT_GE(ms, 900);
    EXPECT_LT(ms, 3000);
}

TEST(AsyncHttpTest, ConnectionRefused) {
    mc::EventLoop loop;
    mc::AsyncHttpClient client(loop, 3);
    Result r;
    client.get(url_for(1, "/"), [&](std::optional<mc::HttpResponse> resp, const std::string& err) {
        r.done = true;
        r.resp = std::move(resp);
        r.err = err;
    });
    ASSERT_TRUE(run_until(loop, client, [&] { return r.done; }));
    EXPECT_FALSE(r.resp);
    EXPECT_NE(r.err.find("connect"), std::string::npos) << r.err;
}

TEST(AsyncHttpTest, HttpsIsRejectedAsynchronously) {
    mc::EventLoop loop;
    mc::AsyncHttpClient client(loop, 3);
    mc::Url u = url_for(443, "/");
    u.scheme = "https";
    Result r;
    client.get(u, [&](std::optional<mc::HttpResponse> resp, const std::string& err) {
        r.done = true;
        r.resp = std::move(resp);
        r.err = err;
    });
    EXPECT_FALSE(r.done);  // 即使是立刻失败，也要等 tick() 才回调
    ASSERT_TRUE(run_until(loop, client, [&] { return r.done; }));
    EXPECT_FALSE(r.resp);
}