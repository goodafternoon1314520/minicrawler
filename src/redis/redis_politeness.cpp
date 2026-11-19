#include "redis/redis_politeness.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace mc {
    namespace {

// 与 MemoryRateLimiter 同一套算法，但状态存在 Redis，时间用 Redis 的 TIME，
// 所以多个进程、多台机器看到的是同一个桶、同一个时钟。
// KEYS[1]=桶  ARGV[1]=rate(个/秒)  ARGV[2]=burst
// 返回 0 = 拿到令牌；>0 = 还要等这么多毫秒（不消耗令牌）
        const char* kBucketScript = R"lua(
local t = redis.call('TIME')
local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local rate = tonumber(ARGV[1])
local burst = tonumber(ARGV[2])
local b = redis.call('HMGET', KEYS[1], 'tokens', 'ts')
local tokens = tonumber(b[1])
local ts = tonumber(b[2])
if tokens == nil or ts == nil then
  tokens = burst
  ts = now
end
local delta = now - ts
if delta < 0 then delta = 0 end
tokens = math.min(burst, tokens + delta * rate / 1000)
local wait = 0
if tokens >= 1 then
  tokens = tokens - 1
else
  wait = math.ceil((1 - tokens) * 1000 / rate)
end
redis.call('HSET', KEYS[1], 'tokens', tostring(tokens), 'ts', tostring(now))
redis.call('PEXPIRE', KEYS[1], math.ceil(burst * 1000 / rate) + 1000)
return wait
)lua";

    }  // namespace

    long RedisRateLimiter::acquire(const std::string& host, double rate, double burst) {
        if (!(rate > 0)) return 0;
        burst = std::max(1.0, burst);

        char r[32], b[32];
        std::snprintf(r, sizeof r, "%.6f", rate);
        std::snprintf(b, sizeof b, "%.6f", burst);

        RespValue v;
        std::string err;
        // 注意这里必须显式构造 std::string：两个 char* 放进花括号会被当成"迭代器区间"
        if (!rc_.eval(kBucketScript, {ns_ + ":rl:" + host}, {std::string(r), std::string(b)}, &v, &err)) {
            std::cerr << "[redis] rate limiter: " << err << "\n";
            return 1000;  // 出错时"关闭"：让调用方等一会儿，而不是不限速地冲向别人的网站
        }
        return v.type == RespValue::Type::kInteger ? static_cast<long>(v.integer) : 1000;
    }

    bool RedisRobotsStore::load(const std::string& host, std::string* blob) {
        RespValue v;
        std::string err;
        if (!rc_.command({"GET", ns_ + ":robots:" + host}, &v, &err)) {
            std::cerr << "[redis] robots load: " << err << "\n";
            return false;
        }
        if (v.type != RespValue::Type::kBulk) return false;
        *blob = std::move(v.str);
        return true;
    }

    void RedisRobotsStore::save(const std::string& host, const std::string& blob, int ttl_sec) {
        std::string err;
        if (!rc_.command({"SET", ns_ + ":robots:" + host, blob, "EX", std::to_string(std::max(1, ttl_sec))}, nullptr,
                         &err))
            std::cerr << "[redis] robots save: " << err << "\n";
    }

}  // namespace mc