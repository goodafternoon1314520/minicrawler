#include "crawler/crawler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <thread>

#include "net/http_client.h"
#include "parser/html_links.h"

namespace mc {
    namespace {

        bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

        bool is_html(const HttpResponse& r) {
            auto it = r.headers.find("content-type");
            if (it == r.headers.end()) return false;
            std::string v = it->second;
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return v.find("text/html") != std::string::npos;
        }

    }  // namespace

    Fetcher default_fetcher(int timeout_sec) {
        return [timeout_sec](const Url& u, std::string* err) { return http_get(u, err, timeout_sec); };
    }

    Crawler::Crawler(CrawlOptions opt, Fetcher fetcher, Frontier* frontier, PageSink* sink)
            : opt_(opt), fetch_(std::move(fetcher)), frontier_(frontier), sink_(sink) {}

    CrawlStats Crawler::run(const Url& seed, PageCallback on_page) {
        CrawlStats stats;
        MemoryFrontier local;
        Frontier& frontier = frontier_ ? *frontier_ : local;

        // 续爬时种子已经在库里，push 返回 false，没有副作用
        frontier.push(seed, 0);

        size_t attempted = 0;
        while (attempted < opt_.max_pages) {
            std::optional<Task> task = frontier.pop();
            if (!task) break;
            ++attempted;
            const int depth = task->depth;

            // 礼貌延迟放在请求之前，这样最后一个页面抓完不会白等
            if (attempted > 1 && opt_.delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(opt_.delay_ms));

            // ---- 抓取（手动跟随重定向）----
            Url cur = task->url;
            std::string err;
            std::optional<HttpResponse> resp;
            for (int hop = 0; hop <= opt_.max_redirects; ++hop) {
                resp = fetch_(cur, &err);
                if (!resp || !is_redirect(resp->status)) break;
                auto loc = resp->headers.find("location");
                if (loc == resp->headers.end()) break;
                auto next = resolve_url(cur, loc->second);
                if (!next || next->scheme != "http") break;
                if (opt_.same_host_only && next->host != seed.host) break;
                cur = *next;
                frontier.mark_seen(cur, depth);  // 重定向目标也算访问过
            }

            PageResult pr;
            pr.url = cur.to_string();
            pr.depth = depth;

            if (!resp) {
                pr.error = err;
                ++stats.failed;
            } else {
                pr.status = resp->status;
                pr.bytes = resp->body.size();
                ++stats.fetched;

                // 1) 先存页面（幂等 upsert，重复执行无害）
                if (sink_) {
                    PageRecord rec;
                    rec.url = pr.url;
                    rec.depth = depth;
                    rec.status = resp->status;
                    auto ct = resp->headers.find("content-type");
                    if (ct != resp->headers.end()) rec.content_type = ct->second;
                    rec.body = resp->body;
                    std::string serr;
                    if (!sink_->save(rec, &serr)) {
                        ++stats.store_failed;
                        std::cerr << "[store] " << rec.url << ": " << serr << "\n";
                    }
                }

                // 2) 再把新链接入队（INSERT 幂等，重复执行无害）
                if (resp->status == 200 && is_html(*resp) && depth < opt_.max_depth) {
                    auto links = extract_links(resp->body);
                    pr.links_found = links.size();
                    for (const auto& raw : links) {
                        auto next = resolve_url(cur, raw);
                        if (!next) continue;
                        if (next->scheme != "http") { ++stats.skipped_unsupported; continue; }
                        if (opt_.same_host_only && next->host != seed.host) continue;
                        frontier.push(*next, depth + 1);
                    }
                }
            }

            // 3) 最后才标记任务完成。
            // 如果前两步之间崩溃，任务仍是 in_progress，重启后会被重做；因为前两步幂等，重做是安全的。
            frontier.complete(task->url, resp.has_value(), pr.error);
            if (on_page) on_page(pr);
        }
        return stats;
    }

}  // namespace mc