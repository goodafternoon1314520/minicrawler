#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "crawler/frontier.h"
#include "redis/redis_client.h"

namespace mc {
    struct RedisFrontierOptions {
        std::string ns = "mc";          // key前缀，不同任务用不同前缀互不干扰
        std::string worker_id = "w1";   // 同时运行的每一个进程必须不同，重启是用同一个id才能认领自己遗留的任务
        int hb_ttl_sec = 15;            // 心跳有效期：进程挂了之后，别人最多等这么久才会回收他任务
    };

    // 约定：同一个RedisClient同一时间只被一个线程使用（和Db的约定一样）
    class RedisFrontier : public Frontier {
    public:
        RedisFrontier(RedisClient& rc, RedisFrontierOptions opt);

        // 占用worker id，若同id的心跳还活着，最多等待ttl+1秒（给刚被kill的旧进程过期的时间），仍被占用则失败
        bool register_worker(std::string* err);
        void unregister_worker();

        // 把自己上次遗留的任务，以及已死亡worker遗留的任务放回待办队列，返回搬回的数量
        size_t recover();

        TaskCounts counts();

        bool push(const Url& url, int depth) override;
        void mark_seen(const Url& url, int depth) override;
        std::optional<Task> pop() override;
        void complete(const Url& url, bool ok, const std::string& error) override;
        bool others_active() override;

    private:
        using Clock = std::chrono::steady_clock;

        std::string key(const std::string& suffix) const { return opt_.ns + ":" + suffix; }
        std::string proc_key(const std::string& id) const { return key("proc:" + id); }
        std::string hb_key(const std::string& id) const { return key("hb:" + id); }

        size_t tick();                      // 到点就发心跳并回收死亡worker的任务，返回回收数量
        size_t move_all(const std::string& from_list);
        size_t reclaim_dead_workers();
        std::vector<std::string> workers();
        bool exists(const std::string& k);
        int64_t llen(const std::string& k);

        RedisClient& rc_;
        RedisFrontierOptions opt_;
        bool registered_ = false;
        std::chrono::seconds tick_interval_;
        Clock::time_point last_tick_{};
        // url -> 他在processing列表里的原始元素，complete()要靠他精确LREM
        // 只在使用RedisClient的那个线程里访问，所以不需要锁
        std::unordered_map<std::string, std::string> in_flight_;
    };
}