#include "crawler/storage_worker.h"

#include <iostream>

namespace mc {
    StorageWorker::StorageWorker(Frontier& frontier, PageSink* sink, size_t pop_budget, size_t prefetch, std::function<void()> wake)
      : frontier_(frontier),
        sink_(sink),
        budget_(pop_budget),
        prefetch_(prefetch),
        wake_(std::move(wake)) {}

    StorageWorker::~StorageWorker() { stop(); }

    void StorageWorker::start() {
        thread_ = std::thread([this] { run(); });
    }

    void StorageWorker::stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    void StorageWorker::post(Command c) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            cmds_.push_back(std::move(c));
            exhausted_ = false;             // 新命令可能带来新任务，之前的取空结论作废
        }
        cv_.notify_one();
    }

    bool StorageWorker::need_fill_locked() const {
        return !exhausted_ && popped_ < budget_ && ready_.size() < prefetch_;
    }

    bool StorageWorker::take(Task* out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (ready_.empty()) return false;
        *out = std::move(ready_.front());
        ready_.pop_front();
        cv_.notify_one();           // 预取队列出现空位，叫worker去补
        return true;
    }

    void StorageWorker::save(PageRecord rec) {
        if (!sink_) return;
        post([this, rec = std::move(rec)] {
            std::string err;
            if (!sink_->save(rec, &err)) {
                ++store_failed_;
                std::cerr << "[store] " << rec.url << ": " << err << std::endl;
            }
        });
    }

    void StorageWorker::push_links(std::vector<std::pair<Url, int>> links) {
        post([this, links = std::move(links)] {
            for (const auto& l : links)
                frontier_.push(l.first, l.second);
        });
    }

    void StorageWorker::mark_seen(const mc::Url &url, int depth) {
        post([this, url, depth] {
            frontier_.mark_seen(url, depth);
        });
    }

    void StorageWorker::complete(const Url& url, bool ok, std::string error) {
        post([this, url, ok, error = std::move(error)] {
            frontier_.complete(url, ok, error);
        });
    }

    bool StorageWorker::drained() {
        std::lock_guard<std::mutex> lk(mu_);
        const bool no_more_tasks = exhausted_ || popped_ >= budget_;
        return no_more_tasks && cmds_.empty() && !busy_ && ready_.empty();
    }

    void StorageWorker::run() {
        std::unique_lock<std::mutex> lk(mu_);
        while (true) {
            cv_.wait(lk, [this] { return stop_ || !cmds_.empty() || need_fill_locked(); });

            // 1. 命令优先：他们产生新任务，释放在途状态
            if (!cmds_.empty()) {
                Command c = std::move(cmds_.front());
                cmds_.pop_front();
                busy_ = true;
                lk.unlock();
                c();            // 可能阻塞在Mysql上，所以不能持锁
                lk.lock();
                busy_ = false;
                if (cmds_.empty()) {
                    lk.unlock();
                    if (wake_)
                        wake_();            // 状态可能变成drained，叫醒事件循环检查
                    lk.lock();
                }
                continue;
            }
            if (stop_) break;

            // 2. 预取任务
            if (need_fill_locked()) {
                lk.unlock();
                std::optional<Task> t = frontier_.pop();        // 阻塞调用，同样不持续
                lk.lock();
                if (t) {
                    ready_.push_back(std::move(*t));
                    ++popped_;
                }
                else if (cmds_.empty())
                    exhausted_ = true;                          // pop期间若来了新命令，就不能下这个结论
            }
            lk.unlock();
            if (wake_)
                wake_();
            lk.lock();
        }
    }
}