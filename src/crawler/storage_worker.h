#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "crawler/frontier.h"
#include "crawler/page_sink.h"

namespace mc {
    class StorageWorker {
    public:
        // pop_budget：最多从frontier取多少个任务（对应max_pages，保证不会多领任务）
        // prefetch：预取队列的目标长度
        // wake：worker状态变化时调用，用来唤醒事件循环（会在worker线程里被调用）
        StorageWorker(Frontier& frontier, PageSink* sink, size_t pop_budget, size_t prefetch, std::function<void()> wake);
        ~StorageWorker();

        void start();
        void stop();        // 先执行完已排队的命令，再推出并join

        // ------------------以下由事件循环线程调用，都不会阻塞在数据库上-------------
        bool take(Task* out);
        void save(PageRecord rec);
        void push_links(std::vector<std::pair<Url, int>> links);
        void mark_seen(const Url& url, int depth);
        void complete(const Url& url, bool ok, std::string error);

        // 命令执行完，预取队列空，frontier也没有更多任务
        bool drained();
        size_t store_failed() const { return store_failed_.load(); }

    private:
        using Command = std::function<void()>;
        void post(Command c);
        void run();
        bool need_fill_locked() const;

        Frontier& frontier_;
        PageSink* sink_;
        const size_t budget_;
        const size_t prefetch_;
        std::function<void()> wake_;

        std::mutex mu_;
        std::condition_variable cv_;
        std::deque<Command> cmds_;
        std::deque<Task> ready_;
        size_t popped_ = 0;
        bool busy_ = false;             // 正在执行某条命令
        bool exhausted_ = false;        // 最后一次pop返回空，且之后没有新命令
        bool stop_ = false;
        std::atomic<size_t> store_failed_{0};
        std::thread thread_;
    };
}