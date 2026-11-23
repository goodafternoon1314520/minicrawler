#include "redis/redis_frontier.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace mc {
    namespace {
        void log_err(const char *where, const std::string &err) {
            std::cerr << "[redis] " << where << ": " << err << std::endl;
        }

        std::string make_item(int depth, int attempt, const std::string &url) {
            return std::to_string(depth) + "/" + std::to_string(attempt) + " " + url;
        }

        // 去重 + 入队必须原子，否则SADD成功后，LPUSH之前崩溃，这个URL就永远丢了
        // KEYS[1]=seen KEY[2]=pending ARGV[1]=url ARGV[2]=item
        // SET里存shal(url)：定长（40字节），不管URL多长，内存可预测
        const char *kPushScript = R"lua(
        if redis.call('SADD', KEYS[1], redis.sha1hex(ARGV[1])) == 1 then
            redis.call('LPUSH', KEYS[2], ARGV[2])
            return 1
        end
        return 0
        )lua";

        const char *kMarkSeenScript = R"lua(
        return redis.call('SADD', KEYS[1], redis.sha1hex(ARGV[1]))
        )lua";

        // KEYS[1]=proc:<我> KEYS[2]=n_done或n_failed KEYS[3]=errors
        // ARGV[1]=item（空串表示不知道） ARGV[2]=error（空串表示成功） ARGV[3]=url
        const char *kCompleteScript = R"lua(
        if ARGV[1] ~= '' then redis.call('LREM', KEYS[1], 1, ARGV[1]) end
        redis.call('INCR', KEYS[2])
        if ARGV[2] ~= '' then redis.call('HSET', KEYS[3], ARGV[3], ARGV[2]) end
        return 1
        )lua";

        // 失败后安排重试：从"处理中"移走 + 放进延迟队列，必须原子。
        // KEYS[1]=proc:<我>  KEYS[2]=delayed   ARGV[1]=旧 item（空串=不知道）  ARGV[2]=新 item  ARGV[3]=延迟毫秒
        const char *kRetryScript = R"lua(
        local t = redis.call('TIME')
        local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
        if ARGV[1] ~= '' then redis.call('LREM', KEYS[1], 1, ARGV[1]) end
        redis.call('ZADD', KEYS[2], now + tonumber(ARGV[3]), ARGV[2])
        return 1
        )lua";

        // 把到期的重试任务搬回待办队列。KEYS[1]=delayed  KEYS[2]=pending
        const char *kPromoteScript = R"lua(
        local t = redis.call('TIME')
        local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
        local due = redis.call('ZRANGEBYSCORE', KEYS[1], '-inf', now, 'LIMIT', 0, 200)
        for _, m in ipairs(due) do
        redis.call('LPUSH', KEYS[2], m)
        redis.call('ZREM', KEYS[1], m)
        end
        return #due
        )lua";
    }

    RedisFrontier::RedisFrontier(RedisClient &rc, RedisFrontierOptions opt) : rc_(rc), opt_(std::move(opt)), tick_interval_(std::max(1, opt_.hb_ttl_sec / 3)){
        opt_.hb_ttl_sec = std::max(1, opt_.hb_ttl_sec);
    }

    // ---------------worker登记与心跳-----------------
    bool RedisFrontier::register_worker(std::string *err) {
        std::string local;
        if (!err)
            err = &local;
        const auto deadline = Clock::now() + std::chrono::seconds(opt_.hb_ttl_sec + 1);
        while (true) {
            RespValue r;
            // SET key 1 NX EX ttl：key不存在才设置成功（回复 +OK），已存在回复nil
            if (!rc_.command({"SET", hb_key(opt_.worker_id), "1", "NX", "EX", std::to_string(opt_.hb_ttl_sec)}, &r, err))
                return false;
            if (r.type == RespValue::Type::kSimple)
                break;
            if (Clock::now() >= deadline) {
                *err = "worker id '" + opt_.worker_id + "' is in use by a live process (use --worker-id)";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        RespValue r;
        if (!rc_.command({"SADD", key("workers"), opt_.worker_id}, &r, err))
            return false;
        registered_ = true;
        last_tick_ = Clock::now();
        return true;
    }

    void RedisFrontier::unregister_worker() {
        if (registered_)
            return;
        RespValue r;
        std::string err;
        rc_.command({"DEL", hb_key(opt_.worker_id)}, &r, &err);         // 只删心跳，workers集合里的残留有别人清理
    }

    size_t RedisFrontier::tick() {
        if (!registered_)
            return 0;
        const auto now = Clock::now();
        if (now - last_tick_ < tick_interval_)
            return 0;
        last_tick_ = now;

        RespValue r;
        std::string err;
        if (!rc_.command({"SET", hb_key(opt_.worker_id), "1", "EX", std::to_string(opt_.hb_ttl_sec)}, &r, &err))
            log_err("heartbeat", err);
        // 每次心跳顺带把自己重新登记，防止曾被误判为死亡而从worker里被删除
        rc_.command({"SADD", key("workers"), opt_.worker_id}, &r, &err);
        return reclaim_dead_workers();
    }

    std::vector<std::string> RedisFrontier::workers() {
        std::vector<std::string> out;
        RespValue r;
        std::string err;
        if (!rc_.command({"SMEMBERS", key("workers")}, &r, &err)) {
            log_err("workers", err);
            return out;
        }
        for (const auto& v : r.array)
            out.push_back(v.str);
        return out;
    }

    bool RedisFrontier::exists(const std::string &k) {
        RespValue r;
        std::string err;
        if (!rc_.command({"EXISTS", k}, &r, &err)) {
            log_err("exists", err);
            return true;                // 查不出来就当他还活着，不要误回收
        }
        return r.integer;
    }

    int64_t RedisFrontier::llen(const std::string& k) {
        RespValue r;
        std::string err;
        if (!rc_.command({"LLEN", k}, &r, &err)) {
            log_err("llen", err);
            return 0;
        }
        return r.integer;
    }

    size_t RedisFrontier::move_all(const std::string& from_list) {
        size_t n = 0;
        while (true) {
            RespValue r;
            std::string err;
            // 逐个原子搬运：任何时刻每个任务都在某个列表里，中途崩溃也不会丢
            if (!rc_.command({"RPOPLPUSH", from_list, key("pending")}, &r, &err)) {
                log_err("move_all", err);
                break;
            }
            if (r.type != RespValue::Type::kBulk)
                break;              // nil：搬完了
            ++n;
        }
        return n;
    }

    size_t RedisFrontier::reclaim_dead_workers() {
        size_t n = 0;
        for (const std::string& id : workers()) {
            if (id == opt_.worker_id)
                continue;
            if (exists(hb_key(id)))
                continue;                       // 还活着
            n += move_all(proc_key(id));
            RespValue r;
            std::string err;
            rc_.command({"SREM", key("workers"), id}, &r, &err);
        }
        return n;
    }

    size_t RedisFrontier::recover() {
        size_t n = move_all(proc_key(opt_.worker_id));          // 自己上次崩溃遗留的
        n += reclaim_dead_workers();                                       // 别人崩溃遗留的
        return n;
    }

    bool  RedisFrontier::others_active() {
        if (tick() > 0)
            return true;                    // 刚回收了任务，队列里又有活了
        if (zcard(key("delayed")) > 0)      // 还有任务在等着重试，现在收工太早
            return true;
        for (const std::string& id : workers()) {
            if (id == opt_.worker_id)
                continue;
            if (!exists(hb_key(id)))
                continue;
            if (llen(proc_key(id)) > 0)
                return true;                // 活着，而且手里有任务
        }
        return false;
    }

    // --------------Frontier接口-----------------
    bool RedisFrontier::push(const Url& url, int depth) {
        tick();
        const std::string s = url.to_string();
        if (s.empty() || s.size() > 2048)
            return false;
        RespValue r;
        std::string err;
        if (!rc_.eval(kPushScript, {key("seen"), key("pending")}, {s, make_item(depth, 0, s)}, &r, &err)) {
            log_err("push", err);
            return false;
        }
        return r.type == RespValue::Type::kInteger && r.integer == 1;
    }

    void RedisFrontier::mark_seen(const mc::Url &url, int) {
        tick();
        RespValue r;
        std::string err;
        if (!rc_.eval(kMarkSeenScript, {key("seen")}, {url.to_string()}, &r, &err))
            log_err("mark_seen", err);
    }

    std::optional<Task> RedisFrontier::pop() {
        tick();
        bool promoted = false;
        if (Clock::now() - last_promote_ >= std::chrono::milliseconds(250)) {  // 定期把到期的重试放回队列
            promote_due();
            promoted = true;
        }
        while (true) {
            RespValue r;
            std::string err;
            if (!rc_.command({"RPOPLPUSH", key("pending"), proc_key(opt_.worker_id)}, &r, &err)) {
                log_err("pop", err);
                return std::nullopt;
            }
            if (r.type != RespValue::Type::kBulk) {
                // 队列空了：趁机看看有没有到期的重试（上面已经看过就不重复）
                if (!promoted) {
                    promoted = true;
                    if (promote_due() > 0) continue;
                }
                return std::nullopt;
            }

            const std::string& item = r.str;
            std::optional<Url> url;
            int depth = 0, attempt = 0;
            // 格式 "深度/次数 URL"；旧格式 "深度 URL" 也认（次数按 0）
            if (size_t sp = item.find(' '); sp != std::string::npos && sp > 0 && sp <= 24) {
                const std::string head = item.substr(0, sp);
                const size_t slash = head.find('/');
                depth = static_cast<int>(std::strtol(head.substr(0, slash).c_str(), nullptr, 10));
                if (slash != std::string::npos)
                    attempt = static_cast<int>(std::strtol(head.substr(slash + 1).c_str(), nullptr, 10));
                url = parse_url(item.substr(sp + 1));
            }
            if (!url) {
                log_err("pop", "malformed queue item dropped");
                rc_.command({"LREM", proc_key(opt_.worker_id), "1", item}, nullptr, nullptr);
                continue;
            }
            in_flight_[url->to_string()] = item;
            return Task{*url, depth, attempt};
        }
    }

    void RedisFrontier::complete(const Url& url, bool ok, const std::string& error) {
        tick();
        const std::string u = url.to_string();
        std::string item;
        if (auto it = in_flight_.find(u); it != in_flight_.end()) {
            item = std::move(it->second);
            in_flight_.erase(it);
        }
        std::string reason;
        if (!ok)
            reason = error.empty() ? "failed" : error.substr(0, 255);

        RespValue r;
        std::string err;
        if (!rc_.eval(kCompleteScript, {proc_key(opt_.worker_id), key(ok ? "n_done" : "n_failed"), key("errors")}, {item, reason, u}, &r, &err))
            log_err("complete", err);
    }

    TaskCounts RedisFrontier::counts() {
        TaskCounts c;
        c.pending = static_cast<uint64_t>(llen(key("pending")));
        c.delayed = static_cast<uint64_t>(zcard(key("delayed")));
        for (const std::string &id: workers())
            c.in_progress += static_cast<uint64_t>(llen(proc_key(id)));

        auto get_counter = [&](const std::string &k) -> uint64_t {
            RespValue r;
            std::string err;
            if (!rc_.command({"GET", k}, &r, &err) || r.type != RespValue::Type::kBulk)
                return 0;
            return std::strtoull(r.str.c_str(), nullptr, 10);
        };
        c.done = get_counter(key("n_done"));
        c.failed = get_counter(key("n_failed"));
        return c;
    }

    int64_t RedisFrontier::zcard(const std::string& k) {
        RespValue r;
        std::string err;
        if (!rc_.command({"ZCARD", k}, &r, &err)) {
            log_err("zcard", err);
            return 0;
        }
        return r.integer;
    }

    size_t RedisFrontier::promote_due() {
        last_promote_ = Clock::now();
        RespValue r;
        std::string err;
        if (!rc_.eval(kPromoteScript, {key("delayed"), key("pending")}, {}, &r, &err)) {
            log_err("promote", err);
            return 0;
        }
        return r.type == RespValue::Type::kInteger ? static_cast<size_t>(r.integer) : 0;
    }

    void RedisFrontier::retry_later(const Url& url, int depth, int attempt, long delay_ms, const std::string&) {
        tick();
        const std::string u = url.to_string();
        std::string old_item;
        if (auto it = in_flight_.find(u); it != in_flight_.end()) {
            old_item = std::move(it->second);
            in_flight_.erase(it);
        }
        RespValue r;
        std::string err;
        if (!rc_.eval(kRetryScript, {proc_key(opt_.worker_id), key("delayed")},
                      {old_item, make_item(depth, attempt, u), std::to_string(std::max(0L, delay_ms))}, &r, &err))
            log_err("retry_later", err);
    }
}