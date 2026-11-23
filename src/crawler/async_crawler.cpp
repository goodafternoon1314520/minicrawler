#include "crawler/async_crawler.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "crawler/crawl_util.h"
#include "crawler/robots.h"
#include "crawler/storage_worker.h"
#include "net/async_http.h"
#include "net/event_loop.h"
#include "net/fd.h"
#include "parser/html_links.h"

namespace mc {
    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr const char* kRobotsToken = "minicrawler";
        constexpr size_t kMaxRobotsBytes = 500 * 1024;
        constexpr int kMaxRobotsRedirects = 3;

        std::string host_key(const Url& u) { return u.host + ":" + std::to_string(u.port); }
        std::string target_of(const Url& u) { return u.query.empty() ? u.path : u.path + "?" + u.query; }

// robots 缓存里存的 blob：首字符是类型，A=全放行  D=全禁止  T=后面跟 robots.txt 原文
        RobotsPolicy policy_from_blob(const std::string& b) {
            if (b.empty() || b[0] == 'A') return RobotsPolicy::allow_all();
            if (b[0] == 'D') return RobotsPolicy::disallow_all();
            return RobotsPolicy::parse(b.substr(1), kRobotsToken);
        }

        struct Job {
            Task task;
            std::string host;
            Clock::time_point started;  // [Step 7] 用来算这次请求的延迟
        };

        enum class RobotsState { kUnknown, kFetching, kReady };

        struct HostState {
            int in_flight = 0;
            Clock::time_point next_start{};
            Clock::time_point not_before{};
            RobotsState robots = RobotsState::kUnknown;
            RobotsPolicy policy;
            Clock::time_point robots_expiry{};
        };

        class Run {
        public:
            Run(const CrawlOptions& opt, Frontier& frontier, PageSink* sink, RobotsStore& robots,
                RateLimiter& limiter, const Url& seed, PageCallback on_page)
                    : opt_(opt),
                      seed_(seed),
                      on_page_(std::move(on_page)),
                      has_sink_(sink != nullptr),
                      can_retry_(frontier.supports_retry()),  // [Step 7]
                      robots_(robots),
                      limiter_(limiter),
                      client_(loop_, opt.timeout_sec),
                      wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
                      worker_(frontier, sink, opt.max_pages, static_cast<size_t>(opt.concurrency) * 2,
                              [fd = wake_.get()] {
                                  uint64_t one = 1;
                                  ssize_t r = ::write(fd, &one, sizeof one);
                                  (void)r;
                              }) {}

            CrawlStats go() {
                if (!loop_.valid() || !wake_) {
                    std::cerr << "failed to create epoll/eventfd\n";
                    return stats_;
                }
                loop_.add(wake_.get(), EPOLLIN, [this](uint32_t) {
                    uint64_t v;
                    ssize_t r = ::read(wake_.get(), &v, sizeof v);
                    (void)r;
                });
                worker_.start();

                const auto t_start = Clock::now();
                auto last_report = t_start;
                size_t last_total = 0;

                while (true) {
                    client_.tick();
                    schedule();
                    if (in_flight_ == 0 && waiting_.empty() && worker_.drained()) break;

                    if (opt_.progress_sec > 0) {  // [Step 7]
                        const auto now = Clock::now();
                        if (now - last_report >= std::chrono::seconds(opt_.progress_sec))
                            report(now, t_start, &last_report, &last_total);
                    }

                    int timeout_ms = client_.has_deferred() ? 0 : (waiting_.empty() ? 100 : 10);
                    loop_.poll(timeout_ms);
                }
                worker_.stop();
                stats_.store_failed = worker_.store_failed();
                summarize_latency();
                return stats_;
            }

        private:
            struct Limit {
                double rate;
                double burst;
            };

            Limit limit_for(const HostState& hs) const {
                Limit l{opt_.host_rate, std::max(1.0, opt_.host_burst)};
                if (opt_.obey_robots) {
                    if (auto d = hs.policy.crawl_delay()) {
                        const double delay = std::min(*d, opt_.max_crawl_delay);
                        if (delay > 0) {
                            const double r = 1.0 / delay;
                            l.rate = l.rate > 0 ? std::min(l.rate, r) : r;
                            l.burst = 1;
                        }
                    }
                }
                return l;
            }

            // ---------- [Step 7] 指标 ----------

            void report(Clock::time_point now, Clock::time_point t_start, Clock::time_point* last, size_t* last_total) {
                const size_t total = stats_.fetched + stats_.failed;
                const double dt = std::chrono::duration<double>(now - *last).count();
                std::cerr << "[stats] t=" << static_cast<long>(std::chrono::duration<double>(now - t_start).count())
                          << "s fetched=" << stats_.fetched << " failed=" << stats_.failed
                          << " retried=" << stats_.retried << " robots_skipped=" << stats_.skipped_robots
                          << " in_flight=" << in_flight_ << " waiting=" << waiting_.size()
                          << " rate=" << (dt > 0 ? (total - *last_total) / dt : 0.0) << "/s\n";
                *last = now;
                *last_total = total;
            }

            void summarize_latency() {
                if (latencies_.empty()) return;
                std::sort(latencies_.begin(), latencies_.end());
                auto pct = [&](double p) {
                    return latencies_[static_cast<size_t>(p * static_cast<double>(latencies_.size() - 1) + 0.5)];
                };
                stats_.latency_p50_ms = pct(0.50);
                stats_.latency_p95_ms = pct(0.95);
                stats_.latency_max_ms = latencies_.back();
            }

            // ---------- [Step 7] 退避 ----------

            // failures = 这个任务此前已经失败的次数。等待 = min(上限, 基准 * 2^次数) * random(0.5, 1.0)
            long backoff_ms(int failures) {
                double d = static_cast<double>(opt_.retry_base_ms) * std::pow(2.0, std::min(failures, 30));
                d = std::min(d, static_cast<double>(opt_.retry_max_ms));
                std::uniform_real_distribution<double> jitter(0.5, 1.0);  // 抖动：把同时失败的任务打散，避免"惊群"
                return static_cast<long>(d * jitter(rng_));
            }

            // ---------- 调度 ----------

            void schedule() {
                const size_t limit = static_cast<size_t>(opt_.concurrency);
                Task t;
                while (waiting_.size() < limit * 2 && worker_.take(&t)) waiting_.push_back(std::move(t));

                const auto now = Clock::now();
                for (auto it = waiting_.begin(); it != waiting_.end() && in_flight_ < limit;) {
                    const std::string hk = host_key(it->url);
                    HostState& hs = hosts_[hk];

                    if (opt_.obey_robots) {
                        if (hs.robots == RobotsState::kReady && now >= hs.robots_expiry) hs.robots = RobotsState::kUnknown;
                        if (hs.robots == RobotsState::kUnknown) begin_robots(hk, it->url);
                        if (hs.robots != RobotsState::kReady) {
                            ++it;
                            continue;
                        }
                        if (!hs.policy.allowed(target_of(it->url))) {
                            skip_blocked(*it);
                            it = waiting_.erase(it);
                            continue;
                        }
                    }

                    if (hs.in_flight >= opt_.max_per_host || now < hs.next_start || now < hs.not_before) {
                        ++it;
                        continue;
                    }

                    const Limit lim = limit_for(hs);
                    if (lim.rate > 0) {
                        const long wait_ms = limiter_.acquire(hk, lim.rate, lim.burst);
                        if (wait_ms > 0) {
                            hs.not_before = now + std::chrono::milliseconds(wait_ms);
                            ++it;
                            continue;
                        }
                    }

                    Task task = std::move(*it);
                    it = waiting_.erase(it);
                    start(std::move(task), now);
                }
            }

            // ---------- robots.txt ----------

            void begin_robots(const std::string& hk, const Url& u) {
                HostState& hs = hosts_[hk];
                std::string blob;
                if (robots_.load(hk, &blob) && !blob.empty()) {
                    hs.policy = policy_from_blob(blob);
                    hs.robots = RobotsState::kReady;
                    hs.robots_expiry = Clock::now() + std::chrono::seconds(blob[0] == 'D' ? 60 : 600);
                    return;
                }
                hs.robots = RobotsState::kFetching;
                Url ru = u;
                ru.path = "/robots.txt";
                ru.query.clear();
                ++stats_.robots_fetched;
                request_robots(hk, ru, 0);
            }

            void request_robots(const std::string& hk, const Url& ru, int hops) {
                client_.get(ru, [this, hk, ru, hops](std::optional<HttpResponse> resp, const std::string&) {
                    on_robots(hk, ru, hops, std::move(resp));
                });
            }

            void on_robots(const std::string& hk, const Url& ru, int hops, std::optional<HttpResponse> resp) {
                if (resp && is_redirect(resp->status) && hops < kMaxRobotsRedirects) {
                    auto loc = resp->headers.find("location");
                    if (loc != resp->headers.end()) {
                        auto next = resolve_url(ru, loc->second);
                        if (next && next->scheme == "http") {
                            request_robots(hk, *next, hops + 1);
                            return;
                        }
                    }
                }

                std::string blob;
                int ttl = 0;
                if (resp && resp->status >= 200 && resp->status < 300) {
                    blob = "T" + resp->body.substr(0, kMaxRobotsBytes);
                    ttl = 86400;
                } else if (resp && resp->status >= 400 && resp->status < 500) {
                    blob = "A";
                    ttl = 86400;
                } else {
                    blob = "D";
                    ttl = 300;
                }

                HostState& hs = hosts_[hk];
                hs.policy = policy_from_blob(blob);
                hs.robots = RobotsState::kReady;
                hs.robots_expiry = Clock::now() + std::chrono::seconds(std::min(ttl, 600));
                robots_.save(hk, blob, ttl);
            }

            void skip_blocked(const Task& t) {
                static const char* kReason = "blocked by robots.txt";
                ++stats_.skipped_robots;
                PageResult pr;
                pr.url = t.url.to_string();
                pr.depth = t.depth;
                pr.skipped = true;
                pr.error = kReason;
                worker_.complete(t.url, false, kReason);
                if (on_page_) on_page_(pr);
            }

            // ---------- 请求与响应 ----------

            void start(Task task, Clock::time_point now) {
                auto job = std::make_shared<Job>();
                job->host = host_key(task.url);
                job->task = std::move(task);
                job->started = now;

                HostState& hs = hosts_[job->host];
                ++hs.in_flight;
                hs.next_start = now + std::chrono::milliseconds(opt_.delay_ms);
                ++in_flight_;
                stats_.peak_in_flight = std::max(stats_.peak_in_flight, in_flight_);

                client_.get(job->task.url, [this, job](std::optional<HttpResponse> resp, const std::string& err) {
                    on_response(job, std::move(resp), err);
                });
            }

            void on_response(const std::shared_ptr<Job>& job, std::optional<HttpResponse> resp, const std::string& err) {
                const auto now = Clock::now();
                --in_flight_;
                HostState& hs = hosts_[job->host];
                --hs.in_flight;

                // [Step 7] 每次尝试都记录延迟和结果（状态码 0 = 没拿到响应）
                latencies_.push_back(std::chrono::duration<double, std::milli>(now - job->started).count());
                ++stats_.by_status[resp ? resp->status : 0];

                const Task& task = job->task;
                PageResult pr;
                pr.url = task.url.to_string();
                pr.depth = task.depth;
                pr.attempt = task.attempt;

                // ---- [Step 7] 1) 失败分类：没拿到响应，或者服务器说"现在不行" ----
                std::string reason;
                bool retryable = false;
                long retry_after = 0;
                if (!resp) {
                    reason = err;
                    retryable = is_retryable_error(err);
                } else if (is_retryable_status(resp->status)) {
                    reason = "HTTP " + std::to_string(resp->status);
                    retryable = true;
                    if (auto ra = retry_after_ms(*resp)) retry_after = *ra;
                    pr.status = resp->status;
                }

                if (!reason.empty()) {
                    pr.error = reason;
                    // 对方要我们等得比上限还久：与其在这里干等，不如放弃
                    const bool too_long = retry_after > opt_.retry_max_ms;
                    if (can_retry_ && retryable && task.attempt < opt_.max_retries && !too_long) {
                        const long delay = std::max(backoff_ms(task.attempt), retry_after);
                        // 429/503 是对整个站点的信号：这个站点的其他任务也一起退让
                        if (resp && (resp->status == 429 || resp->status == 503))
                            hs.not_before = std::max(hs.not_before, now + std::chrono::milliseconds(delay));
                        ++stats_.retried;
                        pr.retrying = true;
                        pr.retry_in_ms = delay;
                        worker_.retry(task.url, task.depth, task.attempt + 1, delay, reason);
                    } else {
                        ++stats_.failed;
                        worker_.complete(task.url, false, reason);
                    }
                    if (on_page_) on_page_(pr);
                    return;
                }

                // ---- 2) 成功拿到响应（包括 404 这类"确定的坏消息"）----
                pr.status = resp->status;
                pr.bytes = resp->body.size();
                stats_.bytes += resp->body.size();
                ++stats_.fetched;

                std::vector<std::pair<Url, int>> found;
                const bool redirect = is_redirect(resp->status);
                if (redirect) {
                    // 重定向目标当作"新发现的链接"，深度不变。它和普通链接一样要过 frontier 去重、robots 检查和限速。
                    auto loc = resp->headers.find("location");
                    if (loc != resp->headers.end()) {
                        auto next = resolve_url(task.url, loc->second);
                        if (next && next->scheme == "http" && (!opt_.same_host_only || next->host == seed_.host))
                            found.emplace_back(*next, task.depth);
                    }
                } else if (resp->status == 200 && is_html(*resp) && task.depth < opt_.max_depth) {
                    auto links = extract_links(resp->body);
                    pr.links_found = links.size();
                    for (const auto& raw : links) {
                        auto next = resolve_url(task.url, raw);
                        if (!next) continue;
                        if (next->scheme != "http") { ++stats_.skipped_unsupported; continue; }
                        if (opt_.same_host_only && next->host != seed_.host) continue;
                        found.emplace_back(*next, task.depth + 1);
                    }
                }

                // 顺序与 Step 3 一致：存页面 -> 入队新链接 -> 最后标记完成。301 本身不存。
                if (has_sink_ && !redirect) {
                    PageRecord rec;
                    rec.url = pr.url;
                    rec.depth = task.depth;
                    rec.status = resp->status;
                    auto ct = resp->headers.find("content-type");
                    if (ct != resp->headers.end()) rec.content_type = ct->second;
                    rec.body = std::move(resp->body);
                    worker_.save(std::move(rec));
                }
                if (!found.empty()) worker_.push_links(std::move(found));

                worker_.complete(task.url, true, "");
                if (on_page_) on_page_(pr);
            }

            const CrawlOptions& opt_;
            Url seed_;
            PageCallback on_page_;
            bool has_sink_;
            bool can_retry_;
            RobotsStore& robots_;
            RateLimiter& limiter_;
            EventLoop loop_;
            AsyncHttpClient client_;
            Fd wake_;
            StorageWorker worker_;  // 必须声明在 wake_ 之后：析构时先 join 线程，再关 eventfd

            std::deque<Task> waiting_;
            std::unordered_map<std::string, HostState> hosts_;
            size_t in_flight_ = 0;
            CrawlStats stats_;
            std::mt19937 rng_{std::random_device{}()};
            std::vector<double> latencies_;
        };

    }  // namespace

    AsyncCrawler::AsyncCrawler(CrawlOptions opt, Frontier* frontier, PageSink* sink, Politeness pol)
            : opt_(opt), frontier_(frontier), sink_(sink), pol_(pol) {
        opt_.concurrency = std::max(1, opt_.concurrency);
        opt_.max_per_host = std::max(1, opt_.max_per_host);
        opt_.timeout_sec = std::max(1, opt_.timeout_sec);
    }

    CrawlStats AsyncCrawler::run(const Url& seed, PageCallback on_page) {
        MemoryFrontier local_frontier;
        MemoryRobotsStore local_robots;
        MemoryRateLimiter local_limiter;
        Frontier& frontier = frontier_ ? *frontier_ : local_frontier;
        RobotsStore& robots = pol_.robots ? *pol_.robots : local_robots;
        RateLimiter& limiter = pol_.limiter ? *pol_.limiter : local_limiter;

        frontier.push(seed, 0);

        Run r(opt_, frontier, sink_, robots, limiter, seed, std::move(on_page));
        return r.go();
    }

}  // namespace mc