#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>

#include "util/url.h"

namespace mc {

    struct TaskCounts {
        uint64_t pending = 0, in_progress = 0, done = 0, failed = 0;
        uint64_t delayed = 0;  // 等待重试中
    };

    struct Task {
        Url url;
        int depth = 0;
        int attempt = 0;  // 已经失败过几次（0 = 第一次尝试）
    };

// 任务队列接口：负责"待抓列表"和"已见过集合"
    class Frontier {
    public:
        virtual ~Frontier() = default;

        // 新 URL 入队。已经见过（或不可存储）返回 false。
        virtual bool push(const Url& url, int depth) = 0;

        // 只登记"见过"，不入队（用于重定向目标）
        virtual void mark_seen(const Url& url, int depth) = 0;

        // 取下一个待抓任务；没有则返回 nullopt
        virtual std::optional<Task> pop() = 0;

        // 任务结束。ok=false 时 error 记录原因
        virtual void complete(const Url& url, bool ok, const std::string& error) = 0;

        // 队列暂时取空时调用：还有没做完的事吗（别的进程手里的任务，或者等待重试的任务）？
        // 有的话现在退出可能为时过早。
        virtual bool others_active() { return false; }

        // 支持重试的实现返回 true。retry_later 把任务从"处理中"移走，delay_ms 之后重新变成可领取。
        // attempt = 重试后的"已失败次数"。不支持的实现，调用方应改用 complete(失败)。
        virtual bool supports_retry() const { return false; }
        virtual void retry_later(const Url& /*url*/, int /*depth*/, int /*attempt*/, long /*delay_ms*/,
                                 const std::string& /*error*/) {}
    };

// 纯内存实现：进程退出就没了
    class MemoryFrontier : public Frontier {
    public:
        bool push(const Url& url, int depth) override {
            if (!seen_.insert(url.to_string()).second) return false;
            queue_.push_back(Task{url, depth, 0});
            return true;
        }
        void mark_seen(const Url& url, int) override { seen_.insert(url.to_string()); }
        std::optional<Task> pop() override {
            promote_due();
            if (queue_.empty()) return std::nullopt;
            Task t = std::move(queue_.front());
            queue_.pop_front();
            return t;
        }
        void complete(const Url&, bool, const std::string&) override {}
        bool others_active() override { return !delayed_.empty(); }

        bool supports_retry() const override { return true; }
        void retry_later(const Url& url, int depth, int attempt, long delay_ms, const std::string&) override {
            delayed_.emplace(Clock::now() + std::chrono::milliseconds(std::max(0L, delay_ms)),
                             Task{url, depth, attempt});
        }

    private:
        using Clock = std::chrono::steady_clock;

        void promote_due() {
            const auto now = Clock::now();
            while (!delayed_.empty() && delayed_.begin()->first <= now) {
                queue_.push_back(std::move(delayed_.begin()->second));
                delayed_.erase(delayed_.begin());
            }
        }

        std::deque<Task> queue_;
        std::unordered_set<std::string> seen_;
        std::multimap<Clock::time_point, Task> delayed_;  // 按到期时间排序
    };

}  // namespace mc