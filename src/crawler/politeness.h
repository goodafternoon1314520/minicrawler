// 限速器与缓存的接口

#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace mc {

// robots.txt 的缓存。多进程时放 Redis，让整个集群只抓一次。blob 的格式由爬虫自己约定。
    class RobotsStore {
    public:
        virtual ~RobotsStore() = default;
        virtual bool load(const std::string& host, std::string* blob) = 0;
        virtual void save(const std::string& host, const std::string& blob, int ttl_sec) = 0;
    };

// 令牌桶。拿到令牌返回 0；没拿到返回"至少还要等多少毫秒"，并且不消耗令牌。
    class RateLimiter {
    public:
        virtual ~RateLimiter() = default;
        virtual long acquire(const std::string& host, double rate_per_sec, double burst) = 0;
    };

    struct Politeness {
        RobotsStore* robots = nullptr;  // 为空 -> 进程内存实现
        RateLimiter* limiter = nullptr;
    };

    class MemoryRobotsStore : public RobotsStore {
    public:
        bool load(const std::string& host, std::string* blob) override;
        void save(const std::string& host, const std::string& blob, int ttl_sec) override;

    private:
        using Clock = std::chrono::steady_clock;
        std::unordered_map<std::string, std::pair<std::string, Clock::time_point>> map_;
    };

    class MemoryRateLimiter : public RateLimiter {
    public:
        using NowMs = std::function<int64_t()>;  // 可注入时钟：测试时用假时间，结果完全确定
        explicit MemoryRateLimiter(NowMs now = nullptr);
        long acquire(const std::string& host, double rate_per_sec, double burst) override;

    private:
        struct Bucket {
            double tokens = 0;
            int64_t ts = 0;
            bool init = false;
        };
        NowMs now_;
        std::unordered_map<std::string, Bucket> buckets_;
    };

}  // namespace mc