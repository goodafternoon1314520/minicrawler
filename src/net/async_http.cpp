#include "./async_http.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#include <cerrno>
#include <cstring>

#include "./http_request.h"

namespace mc {
    namespace {
        constexpr size_t kMaxResponse = 10 * 1024 * 1024;
    }

    // ----------------发起请求---------------------------
    bool AsyncHttpClient::resolve(const Url& url, Resolved* out, std::string* err) {
        auto it = dns_cache_.find(url.host);
        if (it == dns_cache_.end()) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            // 注意：getaddrinfo是阻塞的，靠缓存缓解，同一站点只解析一次
            int rc = ::getaddrinfo(url.host.c_str(), nullptr, &hints, &res);
            if (rc != 0) {
                *err = std::string("getaddrinfo: ") + ::gai_strerror(rc);
                return false;
            }
            std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(res, ::freeaddrinfo);
            Resolved r;
            r.family = res->ai_family;
            r.len = res->ai_addrlen;
            std::memcpy(&r.ss, res->ai_addr, res->ai_addrlen);
            it = dns_cache_.emplace(url.host, r).first;
        }
        *out = it->second;
        if (out->family == AF_INET)
            reinterpret_cast<sockaddr_in*>(&out->ss)->sin_port = htons(static_cast<uint16_t>(url.port));
        else if (out->family == AF_INET6)
            reinterpret_cast<sockaddr_in6*>(&out->ss)->sin6_port = htons(static_cast<uint16_t>(url.port));
        return true;
    }

    void AsyncHttpClient::get(const Url& url, Callback cb) {
        auto fail = [&](const std::string& msg) { deferred_.emplace_back(std::move(cb), msg); };

        if (url.scheme != "http")
            return fail("only http:// is supported for now");

        Resolved addr;
        std::string err;
        if (!resolve(url, &addr, &err))
            return fail(err);

        Fd fd(::socket(addr.family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!fd)
            return fail(std::string("socket: ") + std::strerror(errno));

        // 非阻塞connect，几乎总是返回-1且errno == EINPROGRESS，表示正在连
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr.ss), addr.len) != 0 && errno != EINPROGRESS)
            return fail(std::string("connect: ") + std::strerror(errno));

        auto c = std::make_unique<Conn>();
        c->state = State::kConnecting;
        c->request = build_http_get_request(url);
        c->deadline = Clock::now() + std::chrono::seconds(timeout_sec_);

        // 连接成功会表现为可写，所以关注EPOLLOUT
        const int fdnum = fd.get();
        if (!loop_.add(fdnum, EPOLLOUT, [this, fdnum](int32_t ev) { on_event(fdnum, ev); }))
            return fail(std::string("epoll_ctl: ") + std::strerror(errno));

        c->fd = std::move(fd);
        c->cb = std::move(cb);
        conns_[fdnum] = std::move(c);
    }

    // -----------------状态机------------------
    void AsyncHttpClient::on_event(int fd, uint32_t ev) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        Conn& c = *it->second;

        if (c.state == State::kConnecting) {
            // 可写不等于连接成功，必须用SO_ERROR取出connect的真实结果
            int soerr = 0;
            socklen_t len = sizeof soerr;
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            if (soerr != 0)
                return finish(fd, std::nullopt, std::string("connect: ") + std::strerror(soerr));
            if (!(ev & EPOLLOUT))
                return finish(fd, std::nullopt, "connect: connection closed");
            c.state = State::kSending;
        }
        if (c.state == State::kSending) {
            if (!send_pending(fd, c))
                return;
        }
        if (c.state == State::kReceiving && (ev & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP)))
            recv_some(fd, c);
    }

    bool AsyncHttpClient::send_pending(int fd, Conn &c) {
        while(c.sent < c.request.size()) {
            ssize_t n = ::send(fd, c.request.data() + c.sent, c.request.size() - c.sent,MSG_NOSIGNAL);
            if (n > 0) {
                c.sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return true;        // 发送缓冲区满，等下次可写
            finish(fd, std::nullopt, std::string("send: ") + std::strerror(errno));
            return false;
        }

        // 请求发完，改为只关注可读，否则EPOLLOUT会一直触发空转
        c.state = State::kReceiving;
        loop_.mod(fd, EPOLLIN | EPOLLRDHUP);
        return true;
    }

    bool AsyncHttpClient::recv_some(int fd, Conn &c) {
        char buf[16384];
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);         // 每次事件只读一次，LT会再通知
        if (n > 0) {
            c.raw.append(buf, static_cast<size_t>(n));
            if (c.raw.size() > kMaxResponse) {
                finish(fd, std::nullopt, "response too large");
                return false;
            }
            return true;
        }
        if (n == 0) {
            // 对端关闭 = 响应结束
            auto resp = parse_http_response(c.raw);
            if (resp)
                finish(fd, std::move(resp), "");
            else
                finish(fd, std::nullopt, "malformed http response");
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return true;
        finish(fd, std::nullopt, std::string("recv: ") + std::strerror(errno));
        return false;
    }

    void AsyncHttpClient::finish(int fd, std::optional<HttpResponse> resp, const std::string &err) {
        auto it = conns_.find(fd);
        if (it == conns_.end())
            return;
        Callback cb = std::move(it->second->cb);
        loop_.del(fd);          // 先从epoll注销
        conns_.erase(it);           // 在析构Conn -> Fd 析构 -> close(fd)
        // 最后才调回调：回调里可能再次get()，此时内部状态已经是一致的
        if (cb)
            cb(std::move(resp), err);
    }

    void AsyncHttpClient::tick() {
        auto pending = std::move(deferred_);
        deferred_.clear();
        for (auto& [cb, err] : pending)
            if (cb) cb(std::nullopt, err);

        // 超时扫描：O(连接数)，几百个连接完全够用，上万个应该换成按deadline排序的堆/时间轮
        const auto now = Clock::now();
        std::vector<int> expired;
        for (auto& [fd, c] : conns_)
            if (now >= c->deadline) expired.push_back(fd);
        for (int fd : expired)
            finish(fd, std::nullopt, "timeout");
    }
}