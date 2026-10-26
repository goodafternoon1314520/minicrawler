#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "./event_loop.h"
#include "./fd.h"
#include "./http_response.h"
#include "util/url.h"

namespace mc {
    class AsyncHttpClient {
    public:
        using Callback = std::function<void(std::optional<HttpResponse>, const std::string& error)>;

        explicit AsyncHttpClient(EventLoop& loop, int timeout_sec = 10) : loop_(loop), timeout_sec_(timeout_sec) {}

        // 发起请求：立即返回：回调一定会被调用恰好一次，且一定发生在get()之后
        void get(const Url& url, Callback cb);

        // 由主循环每轮调用，交付延迟的失败，处理超时
        void tick();

        bool has_deferred() const { return !deferred_.empty(); }
        size_t active() const { return conns_.size(); }

    private:
        using Clock = std::chrono::steady_clock;
        enum class State { kConnecting, kSending, kReceiving };

        struct Conn {
            Fd fd;
            State state = State::kConnecting;
            std::string request;
            size_t sent = 0;
            std::string raw;                // 已收到的原始字节，EOF时一次性解析
            Clock::time_point deadline{};
            Callback cb;
        };
        struct Resolved {
            int family = 0;
            sockaddr_storage ss{};
            socklen_t len = 0;
        };

        bool resolve(const Url& url, Resolved* out, std::string* err);
        void on_event(int fd, uint32_t events);
        bool send_pending(int fd, Conn& c); // 返回false表示链接已结束并被销毁
        bool recv_some(int fd, Conn& c);
        void finish(int fd, std::optional<HttpResponse> resp, const std::string& err);

        EventLoop& loop_;
        int timeout_sec_;
        std::unordered_map<int, std::unique_ptr<Conn>> conns_;
        std::unordered_map<std::string, Resolved> dns_cache_;
        std::vector<std::pair<Callback, std::string>> deferred_;
    };
}