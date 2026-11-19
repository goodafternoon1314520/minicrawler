#include "crawler/politeness.h"

#include <algorithm>
#include <cmath>

namespace mc {

    bool MemoryRobotsStore::load(const std::string& host, std::string* blob) {
        auto it = map_.find(host);
        if (it == map_.end()) return false;
        if (Clock::now() >= it->second.second) {
            map_.erase(it);
            return false;
        }
        *blob = it->second.first;
        return true;
    }

    void MemoryRobotsStore::save(const std::string& host, const std::string& blob, int ttl_sec) {
        map_[host] = {blob, Clock::now() + std::chrono::seconds(std::max(1, ttl_sec))};
    }

    MemoryRateLimiter::MemoryRateLimiter(NowMs now) : now_(std::move(now)) {
        if (!now_) {
            now_ = [] {
                return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            };
        }
    }

    long MemoryRateLimiter::acquire(const std::string& host, double rate, double burst) {
        if (!(rate > 0)) return 0;  // 0 或非法：不限速
        burst = std::max(1.0, burst);
        const int64_t now = now_();

        Bucket& b = buckets_[host];
        if (!b.init) {  // 新桶是满的
            b.init = true;
            b.tokens = burst;
            b.ts = now;
        }
        const double delta = static_cast<double>(std::max<int64_t>(0, now - b.ts));
        b.tokens = std::min(burst, b.tokens + delta * rate / 1000.0);  // 补充令牌，不超过桶容量
        b.ts = now;

        if (b.tokens >= 1.0) {
            b.tokens -= 1.0;
            return 0;
        }
        return static_cast<long>(std::ceil((1.0 - b.tokens) * 1000.0 / rate));
    }

}  // namespace mc