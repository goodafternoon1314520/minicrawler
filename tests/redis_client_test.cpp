#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "redis/redis_client.h"

namespace {

// 一次性服务器：收下请求，然后把 chunks 一块一块发出去（块之间停顿），最后挂起 hang_ms 再关连接
    int start_scripted(const std::vector<std::string>& chunks, std::string* got, std::thread* th,
                       int hang_ms = 0) {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(lfd, 1);
        socklen_t len = sizeof addr;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
        const int port = ntohs(addr.sin_port);

        *th = std::thread([=] {
            int c = ::accept(lfd, nullptr, nullptr);
            char buf[4096];
            ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n > 0) got->assign(buf, static_cast<size_t>(n));
            for (const auto& ch : chunks) {
                ::send(c, ch.data(), ch.size(), MSG_NOSIGNAL);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            if (hang_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hang_ms));
            ::close(c);
            ::close(lfd);
        });
        return port;
    }

    mc::RedisConfig cfg_for(int port, int timeout_sec = 3) {
        mc::RedisConfig c;
        c.host = "127.0.0.1";
        c.port = static_cast<unsigned>(port);
        c.timeout_sec = timeout_sec;
        return c;
    }

}  // namespace

TEST(RedisClientTest, ReassemblesSplitReply) {
    std::string got;
    std::thread th;
    int port = start_scripted({"$5\r\nhe", "llo\r", "\n"}, &got, &th);

    mc::RedisClient c;
    std::string err;
    ASSERT_TRUE(c.connect(cfg_for(port), &err)) << err;
    mc::RespValue v;
    ASSERT_TRUE(c.command({"GET", std::string("k")}, &v, &err)) << err;
    th.join();

    EXPECT_EQ(v.type, mc::RespValue::Type::kBulk);
    EXPECT_EQ(v.str, "hello");
    EXPECT_EQ(got, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
}

TEST(RedisClientTest, ErrorReplyBecomesFailure) {
    std::string got;
    std::thread th;
    int port = start_scripted({"-ERR unknown command\r\n"}, &got, &th);

    mc::RedisClient c;
    std::string err;
    ASSERT_TRUE(c.connect(cfg_for(port), &err)) << err;
    mc::RespValue v;
    EXPECT_FALSE(c.command({"NOPE"}, &v, &err));
    th.join();
    EXPECT_NE(err.find("unknown command"), std::string::npos) << err;
}

TEST(RedisClientTest, ServerClosesWithoutReply) {
    std::string got;
    std::thread th;
    int port = start_scripted({}, &got, &th);

    mc::RedisClient c;
    std::string err;
    ASSERT_TRUE(c.connect(cfg_for(port), &err)) << err;
    EXPECT_FALSE(c.command({"PING"}, nullptr, &err));
    th.join();
    EXPECT_NE(err.find("closed"), std::string::npos) << err;
    EXPECT_FALSE(c.connected());  // 出错后连接被丢弃，下次调用会重连
}

TEST(RedisClientTest, TimesOutOnSilentServer) {
    std::string got;
    std::thread th;
    int port = start_scripted({}, &got, &th, /*hang_ms=*/2500);

    mc::RedisClient c;
    std::string err;
    ASSERT_TRUE(c.connect(cfg_for(port, /*timeout_sec=*/1), &err)) << err;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(c.command({"PING"}, nullptr, &err));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    th.join();
    EXPECT_EQ(err, "timeout");
    EXPECT_GE(ms, 900);
    EXPECT_LT(ms, 2000);
}

TEST(RedisClientTest, ConnectionRefused) {
    mc::RedisClient c;
    std::string err;
    EXPECT_FALSE(c.connect(cfg_for(1), &err));
    EXPECT_NE(err.find("connect"), std::string::npos) << err;
}