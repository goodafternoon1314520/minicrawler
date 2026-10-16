#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet//in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "../src/net/http_client.h"

namespace {
    // 起一个一次性服务器，收到请求后回复reply，返回监听端口
    int start_fake_server(const std::string& reply, std::string* got_request, std::thread* th) {
        int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;      //让内核挑一个空闲端口
        ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(lfd, 1);

        socklen_t len = sizeof addr;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
        int port = ntohs(addr.sin_port);

        *th = std::thread([=] {
            int c = ::accept(lfd, nullptr, nullptr);
            char buf[4096];
            ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n > 0)
                got_request->assign(buf, static_cast<size_t>(n));
            ::send(c, reply.data(), reply.size(), MSG_NOSIGNAL);
            ::close(c);
            ::close(lfd);
        });

        return port;
    }
}

TEST(HttpClientTest, GetChunkedFromLocalServer) {
    std::string req;
    std::thread th;
    int port = start_fake_server("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", &req, &th);

    mc::Url url;
    url.scheme = "http";
    url.host = "127.0.0.1";
    url.port = port;
    url.path = "/hello";
    url.query = "a=1";

    std::string err;
    auto resp = mc::http_get(url, &err, 3);
    th.join();

    ASSERT_TRUE(resp) << err;
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->body, "abc");
    EXPECT_EQ(req.rfind("GET /hello?a=1 HTTP/1.1\r\n", 0), 0u);
    EXPECT_NE(req.find("Host: 127.0.0.1:" + std::to_string(port)), std::string::npos);
}

TEST(HttpClientTest, ConnectionRefused) {
    mc::Url url;
    url.scheme = "http";
    url.host = "127.0.0.1";
    url.port = 1;
    url.path = "/";
    std::string err;
    EXPECT_FALSE(mc::http_get(url, &err, 2));
    EXPECT_FALSE(err.empty());
}

TEST(HttpClientTest, RejectsHttps) {
    mc::Url url;
    url.scheme = "http";
    url.host = "example.com";
    url.port = 443;
    url.path = "/";
    EXPECT_FALSE(mc::http_get(url));
}