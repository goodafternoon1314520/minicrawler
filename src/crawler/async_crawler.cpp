#include "crawler/async_crawler.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <unordered_map>

#include "crawler/crawl_util.h"
#include "crawler/storage_worker.h"
#include "net/async_http.h"
#include "net/event_loop.h"
#include "net/fd.h"
#include "parser/html_links.h"

namespace mc {
    namespace {

        using Clock = std::chrono::steady_clock;

        std::string host_key(const Url& u) { return u.host + ":" + std::to_string(u.port); }

// 一个任务从发起到结束的上下文（重定向时同一个 Job 会被重新发起）
        struct Job {
            Task task;
            Url cur;
            int hops = 0;
            std::string host;
        };

        class Run {
        public:
            Run(const CrawlOptions& opt, Frontier& frontier, PageSink* sink, const Url& seed,
                PageCallback on_page)
                    : opt_(opt),
                      seed_(seed),
                      on_page_(std::move(on_page)),
                      has_sink_(sink != nullptr),
                      client_(loop_, opt.timeout_sec),
                      wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
                      worker_(frontier, sink, opt.max_pages, static_cast<size_t>(opt.concurrency) * 2,
                              [fd = wake_.get()] {
                                  uint64_t one = 1;
                                  ssize_t r = ::write(fd, &one, sizeof one);  // 让 epoll_wait 立刻返回
                                  (void)r;
                              }) {}

            CrawlStats go() {
                if (!loop_.valid() || !wake_) {
                    std::cerr << "failed to create epoll/eventfd\n";
                    return stats_;
                }
                // eventfd 可读 = 存储线程有动静。读出计数把它清零（LT 模式下不读会一直触发）
                loop_.add(wake_.get(), EPOLLIN, [this](uint32_t) {
                    uint64_t v;
                    ssize_t r = ::read(wake_.get(), &v, sizeof v);
                    (void)r;
                });
                worker_.start();

                while (true) {
                    client_.tick();
                    schedule();
                    if (in_flight_ == 0 && waiting_.empty() && worker_.drained()) break;

                    int timeout_ms = client_.has_deferred() ? 0 : (waiting_.empty() ? 100 : 10);
                    loop_.poll(timeout_ms);
                }
                worker_.stop();
                stats_.store_failed = worker_.store_failed();
                return stats_;
            }

        private:
            struct HostState {
                int in_flight = 0;
                Clock::time_point next_start{};
            };

            // 从存储线程取任务，在并发数、站点并发数、站点间隔三重限制下启动请求
            void schedule() {
                const size_t limit = static_cast<size_t>(opt_.concurrency);
                Task t;
                while (waiting_.size() < limit * 2 && worker_.take(&t)) waiting_.push_back(std::move(t));

                const auto now = Clock::now();
                for (auto it = waiting_.begin(); it != waiting_.end() && in_flight_ < limit;) {
                    HostState& hs = hosts_[host_key(it->url)];
                    if (hs.in_flight < opt_.max_per_host && now >= hs.next_start) {
                        Task task = std::move(*it);
                        it = waiting_.erase(it);
                        start(std::move(task), now);
                    } else {
                        ++it;
                    }
                }
            }

            void start(Task task, Clock::time_point now) {
                auto job = std::make_shared<Job>();
                job->host = host_key(task.url);
                job->cur = task.url;
                job->task = std::move(task);

                HostState& hs = hosts_[job->host];
                ++hs.in_flight;
                hs.next_start = now + std::chrono::milliseconds(opt_.delay_ms);
                ++in_flight_;
                stats_.peak_in_flight = std::max(stats_.peak_in_flight, in_flight_);
                issue(job);
            }

            void issue(const std::shared_ptr<Job>& job) {
                client_.get(job->cur, [this, job](std::optional<HttpResponse> resp, const std::string& err) {
                    on_response(job, std::move(resp), err);
                });
            }

            void on_response(const std::shared_ptr<Job>& job, std::optional<HttpResponse> resp,
                             const std::string& err) {
                // ---- 重定向：换个 URL 继续发，并发名额不释放 ----
                if (resp && is_redirect(resp->status) && job->hops < opt_.max_redirects) {
                    auto loc = resp->headers.find("location");
                    if (loc != resp->headers.end()) {
                        auto next = resolve_url(job->cur, loc->second);
                        if (next && next->scheme == "http" &&
                            (!opt_.same_host_only || next->host == seed_.host)) {
                            job->cur = *next;
                            ++job->hops;
                            worker_.mark_seen(job->cur, job->task.depth);
                            issue(job);
                            return;
                        }
                    }
                }

                // ---- 到这里这个任务结束了 ----
                --in_flight_;
                --hosts_[job->host].in_flight;

                PageResult pr;
                pr.url = job->cur.to_string();
                pr.depth = job->task.depth;
                const bool ok = resp.has_value();

                if (!resp) {
                    pr.error = err;
                    ++stats_.failed;
                } else {
                    pr.status = resp->status;
                    pr.bytes = resp->body.size();
                    ++stats_.fetched;

                    const int depth = job->task.depth;
                    std::vector<std::pair<Url, int>> found;
                    if (resp->status == 200 && is_html(*resp) && depth < opt_.max_depth) {
                        auto links = extract_links(resp->body);
                        pr.links_found = links.size();
                        for (const auto& raw : links) {
                            auto next = resolve_url(job->cur, raw);
                            if (!next) continue;
                            if (next->scheme != "http") { ++stats_.skipped_unsupported; continue; }
                            if (opt_.same_host_only && next->host != seed_.host) continue;
                            found.emplace_back(*next, depth + 1);
                        }
                    }

                    // 顺序与 Step 3 一致：存页面 -> 入队新链接 -> 最后标记完成
                    if (has_sink_) {
                        PageRecord rec;
                        rec.url = pr.url;
                        rec.depth = depth;
                        rec.status = resp->status;
                        auto ct = resp->headers.find("content-type");
                        if (ct != resp->headers.end()) rec.content_type = ct->second;
                        rec.body = std::move(resp->body);
                        worker_.save(std::move(rec));
                    }
                    if (!found.empty()) worker_.push_links(std::move(found));
                }

                worker_.complete(job->task.url, ok, pr.error);
                if (on_page_) on_page_(pr);
            }

            const CrawlOptions& opt_;
            Url seed_;
            PageCallback on_page_;
            bool has_sink_;
            EventLoop loop_;
            AsyncHttpClient client_;
            Fd wake_;
            StorageWorker worker_;  // 必须声明在 wake_ 之后：析构时先 join 线程，再关 eventfd

            std::deque<Task> waiting_;  // 已取出但还没满足限流条件的任务
            std::unordered_map<std::string, HostState> hosts_;
            size_t in_flight_ = 0;
            CrawlStats stats_;
        };

    }  // namespace

    AsyncCrawler::AsyncCrawler(CrawlOptions opt, Frontier* frontier, PageSink* sink)
            : opt_(opt), frontier_(frontier), sink_(sink) {
        opt_.concurrency = std::max(1, opt_.concurrency);
        opt_.max_per_host = std::max(1, opt_.max_per_host);
        opt_.timeout_sec = std::max(1, opt_.timeout_sec);
    }

    CrawlStats AsyncCrawler::run(const Url& seed, PageCallback on_page) {
        MemoryFrontier local;
        Frontier& frontier = frontier_ ? *frontier_ : local;
        frontier.push(seed, 0);  // 此时存储线程还没启动，主线程独占 frontier

        Run r(opt_, frontier, sink_, seed, std::move(on_page));
        return r.go();
    }

}  // namespace mc