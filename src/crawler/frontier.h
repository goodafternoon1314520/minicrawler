#pragma once
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>

#include "util/url.h"

namespace mc {

    struct Task {
        Url url;
        int depth = 0;
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
    };

// Step 2 的行为：纯内存，进程退出就没了
    class MemoryFrontier : public Frontier {
    public:
        bool push(const Url& url, int depth) override {
            if (!seen_.insert(url.to_string()).second) return false;
            queue_.push_back(Task{url, depth});
            return true;
        }
        void mark_seen(const Url& url, int) override { seen_.insert(url.to_string()); }
        std::optional<Task> pop() override {
            if (queue_.empty()) return std::nullopt;
            Task t = std::move(queue_.front());
            queue_.pop_front();
            return t;
        }
        void complete(const Url&, bool, const std::string&) override {}

    private:
        std::deque<Task> queue_;
        std::unordered_set<std::string> seen_;
    };

}  // namespace mc