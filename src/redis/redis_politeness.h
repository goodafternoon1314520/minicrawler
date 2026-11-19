// 共享令牌桶与robots缓存

#pragma once
#include <string>

#include "crawler/politeness.h"
#include "redis/redis_client.h"

namespace mc {

// 约定：传入的 RedisClient 只能被一个线程使用（和 RedisFrontier 一样）。
// AsyncCrawler 里它们都在事件循环线程调用，所以要给它们单独一条连接。
    class RedisRateLimiter : public RateLimiter {
    public:
        RedisRateLimiter(RedisClient& rc, std::string ns) : rc_(rc), ns_(std::move(ns)) {}
        long acquire(const std::string& host, double rate_per_sec, double burst) override;

    private:
        RedisClient& rc_;
        std::string ns_;
    };

    class RedisRobotsStore : public RobotsStore {
    public:
        RedisRobotsStore(RedisClient& rc, std::string ns) : rc_(rc), ns_(std::move(ns)) {}
        bool load(const std::string& host, std::string* blob) override;
        void save(const std::string& host, const std::string& blob, int ttl_sec) override;

    private:
        RedisClient& rc_;
        std::string ns_;
    };

}  // namespace mc