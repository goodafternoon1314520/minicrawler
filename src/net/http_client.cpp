#include "./http_client.h"
#include "./fd.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>
#include <memory>

namespace mc {
    namespace {
        constexpr size_t kMaxResponse = 10 * 1024 * 1024;   // 防止被超大响应撑爆内存

        // send可能只发出一部分，必须循环知道发完
        bool send_all(int fd, const std::string& data) {
            size_t off = 0;
            while (off < data.size()) {
                ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                off += static_cast<size_t>(n);
            }
            return true;
        }

        std::string build_request(const Url& url) {
            bool default_port = (url.port == 80);
            std::string target = url.path;
            if (!url.query.empty())
                target += "?" + url.query;

            std::string req;
            req += "GET " + target + " HTTP/1.1\r\n";
            req += "Host: " + url.host + (default_port ? "" : ":" + std::to_string(url.port));
            req += "User-Agent: minicrawler/0.1\r\n";
            req += "Accept: */*\r\n";
            req += "Accept-Encoding: identity\r\n";     // 先不处理gzip
            req += "Connection: close\r\n";             // 服务器发完就关，读到EOF结束
            req += "\r\n";
            return req;
        }
    }

    std::optional<HttpResponse> http_get(const Url& url, std::string* error, int timeout_sec) {
        auto fail = [&](const std::string& msg) -> std::optional<HttpResponse> {
            if (error)
                *error = msg;
            return std::nullopt;
        };
        if (url.scheme != "http")
            return fail("only http:// is supported for now");

        // 1. DNS解析：域名 -> 一个或多个IP
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        int rc = ::getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &res);
        if (rc != 0)
            return fail(std::string("getaddrinfo: ") + ::gai_strerror(rc));
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> res_guard(res, ::freeaddrinfo);

        // 2. 依次尝试每个地址，直到connect成功
        Fd fd;
        int last_errno = 0;
        for (addrinfo* p = res; p; p = p -> ai_next) {
            Fd s(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
            if (!s) {
                last_errno = errno;
                continue;
            }
            timeval tv{timeout_sec, 0};     // 收发超时：Linux上SO_SNDTIMEO同时约束connect
            ::setsockopt(s.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ::setsockopt(s.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

            if (::connect(s.get(), p->ai_addr, p->ai_addrlen) == 0) {
                fd = std::move(s);
                break;
            }
            last_errno = errno;
        }
        if (!fd)
            return fail(std::string("connect: ") + std::strerror(last_errno));

        // 3. 发请求
        if (!send_all(fd.get(), build_request(url)))
            return fail(std::string("send: ") + std::strerror(errno));

        // 4. 读响应直到对端关闭（recv返回0）
        std::string raw;
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(fd.get(), buf, sizeof buf, 0);
            if (n > 0) {
                raw.append(buf, static_cast<size_t>(n));
                if (raw.size() > kMaxResponse)
                    return fail("response to large");
                else if (n == 0)
                    break;
                else {
                    if (errno == EINTR)
                        continue;
                    return fail(std::string("recv: ") + std::strerror(errno));
                }
            }
        }

        // 5. 解析
        auto resp = parse_http_response(raw);
        if (!resp)
            return fail("malformed http response");
        return resp;
    }
}