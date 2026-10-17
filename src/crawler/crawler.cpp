#include "./crawler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <thread>
#include <unordered_set>

#include "../net/http_client.h"
#include "../parser/html_links.h"

namespace mc {
    namespace {
        bool is_redirect(int s) {
            return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
        }

        bool is_html(const HttpResponse& r) {
            auto it = r.headers.find("content-type");
            if (it == r.headers.end())
                return false;
            std::string v = it -> second;
            std::transform(v.begin(), v.end(), v.begin(),[](unsigned char c) { return std::tolower(c); });
            return v.find("text/html") != std::string::npos;
        }
    }

    Fetcher default_fetcher(int timeout_sec) {
        return [timeout_sec](const Url& u, std::string* err) { return http_get(u, err, timeout_sec); };
    }

    Crawler::Crawler(CrawlOptions opt, Fetcher fetcher) : opt_(opt), fetch_(std::move(fetcher)) {}

    CrawlStats Crawler::run(const Url& seed, PageCallback on_page) {
        CrawlStats stats;
        std::deque<std::pair<Url, int>> queue;      // BFS: 先进先出
        std::unordered_set<std::string> seen;       // 已入队的规范化URL

        queue.emplace_back(seed, 0);
        seen.insert(seed.to_string());

        size_t attempted = 0;
        while (!queue.empty() && attempted < opt_.max_pages) {
            auto [url, depth] = queue.front();
            queue.pop_front();
            ++attempted;

            // 抓取
            Url cur = url;
            std::string err;
            std::optional<HttpResponse> resp;
            for (int hop = 0; hop <= opt_.max_redirects; ++hop) {
                resp = fetch_(cur, &err);
                if (!resp || !is_redirect(resp->status))
                    break;
                auto loc = resp -> headers.find("location");
                if (loc == resp->headers.end())
                    break;
                auto next = resolve_url(cur, loc->second);
                if (!next || next->scheme != "http")
                    break;
                if (opt_.same_host_only && next->host != seed.host)
                    break;
                cur = *next;
                seen.insert(cur.to_string());       // 重定向目标也算访问过
            }

            PageResult pr;
            pr.url = cur.to_string();
            pr.depth = depth;

            if (!resp) {
                pr.error = err;
                ++stats.failed;
            }
            else {
                pr.status = resp->status;
                pr.bytes = resp->body.size();
                ++stats.fetched;

                // 只有200的HTML页面才提取连接，且深度没到上限
                if (resp->status == 200 && is_html(*resp) && depth < opt_.max_depth) {
                    auto links = extract_links(resp->body);
                    pr.links_found = links.size();
                    for (const auto& raw : links) {
                        auto next = resolve_url(cur, raw);
                        if (!next)
                            continue;
                        if (next->scheme != "http") {
                            ++stats.skipped_unsupported;
                            continue;
                        }
                        if (opt_.same_host_only && next->host != seed.host)
                            continue;
                        if (!seen.insert(next->to_string()).second)
                            continue;
                        queue.emplace_back(*next, depth + 1);
                    }
                }
            }
            if (on_page)
                on_page(pr);
            if (opt_.delay_ms > 0 && !queue.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(opt_.delay_ms));
        }
        return stats;
    }
}