#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 测试用HTTP服务器：每个连接一个线程
// delay_ms：读到请求后先等多久再回复，hang：读到请求永不回复（测超时）
class FakeServer {
public:
    // 返回ture -> 200并填充body：返回false -> 404
    using Handler = std::function<bool(const std::string& path, std::string* body)>;
    explicit FakeServer(Handler h, int delay_ms = 0, bool hang = false)
        : handler_(std::move(h)), delay_ms_(delay_ms), hang_(hang) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(listen_fd_, 512);
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    ~FakeServer() {
        stop_ = true;
        acceptor_.join();
        for (auto& t : workers_)
            t.join();
        ::close(listen_fd_);
    }

    int port() const { return port_; }
    int max_active() const { return max_active_.load(); }           // 同时处理中的请求峰值
    int total() const { return total_.load(); }
    int hits(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = hits_.find(path);
        return it == hits_.end() ? 0 : it->second;
    }

private:
    void accept_loop() {
        while (!stop_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
                continue;
            int c = ::accept(listen_fd_, nullptr, nullptr);
            if (c < 0)
                continue;
            workers_.emplace_back([this, c] { server(c); });            // 只有acceptor线程碰workers_
        }
    }

    void server(int c) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) {
                ::close(c);
                return;
            }
            req.append(buf, static_cast<size_t>(n));
        }
        std::string path = "/";
        size_t sp1 = req.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? sp1 : req.find(' ', sp1 + 1);
        if (sp2 != std::string::npos)
            path = req.substr(sp1 + 1, sp2 - sp1 - 1);

        int now = ++active_;
        int prev = max_active_.load();
        while (now > prev && !max_active_.compare_exchange_weak(prev, now)) {}
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++hits_[path];
        }
        ++total_;

        for (int waited = 0; !stop_ && (hang_ || waited < delay_ms_); waited += 10)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // 先减计数再发送，客户端收到响应时，这个请求在服务器一定已经不活跃
        --active_;
        if (hang_) {
            ::close(c);
            return;
        }

        std::string body;
        bool ok = handler_(path, &body);
        std::string resp = std::string(ok ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
                            "\r\nContent-Type: text/html\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;

        size_t off = 0;
        while (off < resp.size()) {
            ssize_t n = ::send(c, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
                break;
            off += static_cast<size_t>(n);
        }
        ::close(c);
    }

    Handler handler_;
    int delay_ms_;
    bool hang_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> active_{0}, max_active_{0}, total_{0};
    std::mutex mu_;
    std::map<std::string, int> hits_;
    std::vector<std::thread> workers_;
    std::thread acceptor_;
};