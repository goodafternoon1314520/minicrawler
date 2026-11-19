#include "crawler/robots.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace mc {
    namespace {

        constexpr auto npos = std::string::npos;
        constexpr size_t kMaxPattern = 1024;

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string trim(const std::string& s) {
            size_t b = s.find_first_not_of(" \t\r");
            if (b == npos) return "";
            size_t e = s.find_last_not_of(" \t\r");
            return s.substr(b, e - b + 1);
        }

// pattern 是否匹配 s 的开头（前缀匹配）。'*' 匹配任意串，结尾的 '$' 要求匹配到 s 末尾。
// 用"可达位置集合"的动态规划，O(|pattern| * |s|)，不会像朴素回溯那样被恶意规则拖死。
        bool matches(const std::string& pat, const std::string& s) {
            const bool anchored = !pat.empty() && pat.back() == '$';
            const size_t plen = anchored ? pat.size() - 1 : pat.size();
            std::vector<char> cur(s.size() + 1, 0), nxt(s.size() + 1, 0);
            cur[0] = 1;
            for (size_t i = 0; i < plen; ++i) {
                const char c = pat[i];
                if (c == '*') {
                    size_t j = 0;
                    while (j <= s.size() && !cur[j]) ++j;
                    if (j > s.size()) return false;
                    for (size_t k = j; k <= s.size(); ++k) cur[k] = 1;
                } else {
                    std::fill(nxt.begin(), nxt.end(), 0);
                    bool any = false;
                    for (size_t j = 0; j < s.size(); ++j) {
                        if (cur[j] && s[j] == c) {
                            nxt[j + 1] = 1;
                            any = true;
                        }
                    }
                    if (!any) return false;
                    cur.swap(nxt);
                }
            }
            return anchored ? cur[s.size()] != 0 : true;
        }

    }  // namespace

    RobotsPolicy RobotsPolicy::disallow_all() {
        RobotsPolicy p;
        p.rules_.push_back({false, "/"});
        return p;
    }

    RobotsPolicy RobotsPolicy::parse(const std::string& text, const std::string& agent) {
        struct Group {
            std::vector<std::string> agents;
            std::vector<Rule> rules;
            std::optional<double> delay;
        };
        std::vector<Group> groups;
        bool last_was_agent = false;  // 连续的 User-agent 行属于同一组

        size_t pos = 0;
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;  // 跳过 UTF-8 BOM
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == npos ? npos : nl - pos);
            pos = (nl == npos) ? text.size() : nl + 1;

            if (auto h = line.find('#'); h != npos) line.erase(h);  // 去注释
            line = trim(line);
            if (line.empty()) continue;  // 空行不结束分组
            size_t colon = line.find(':');
            if (colon == npos) continue;  // 不认识的行，忽略

            const std::string key = lower(trim(line.substr(0, colon)));
            const std::string value = trim(line.substr(colon + 1));

            if (key == "user-agent") {
                if (!last_was_agent) groups.emplace_back();
                groups.back().agents.push_back(lower(value));
                last_was_agent = true;
            } else if (key == "allow" || key == "disallow") {
                last_was_agent = false;
                if (groups.empty()) continue;                       // 出现在任何 User-agent 之前：忽略
                if (value.empty() || value.size() > kMaxPattern) continue;  // "Disallow:" 为空 = 不禁止任何东西
                groups.back().rules.push_back({key == "allow", value});
            } else if (key == "crawl-delay") {
                last_was_agent = false;
                if (groups.empty()) continue;
                char* end = nullptr;
                double d = std::strtod(value.c_str(), &end);
                if (!value.empty() && *end == '\0' && std::isfinite(d) && d >= 0) groups.back().delay = d;
            }
            // Sitemap 等其他行：忽略，也不影响分组状态
        }

        const std::string me = lower(agent);
        auto pick = [&](const std::string& who) {
            std::vector<const Group*> v;
            for (const Group& g : groups)
                if (std::find(g.agents.begin(), g.agents.end(), who) != g.agents.end()) v.push_back(&g);
            return v;
        };
        auto chosen = pick(me);
        if (chosen.empty()) chosen = pick("*");

        RobotsPolicy p;
        for (const Group* g : chosen) {  // 同一 agent 的多个分组要合并
            p.rules_.insert(p.rules_.end(), g->rules.begin(), g->rules.end());
            if (g->delay && !p.crawl_delay_) p.crawl_delay_ = g->delay;
        }
        return p;
    }

    bool RobotsPolicy::allowed(const std::string& path_and_query) const {
        const std::string p = path_and_query.empty() ? "/" : path_and_query;
        bool matched = false, best_allow = true;
        size_t best_len = 0;
        for (const Rule& r : rules_) {
            if (!matches(r.pattern, p)) continue;
            const size_t len = r.pattern.size();
            if (!matched || len > best_len || (len == best_len && r.allow && !best_allow)) {
                matched = true;
                best_len = len;
                best_allow = r.allow;
            }
        }
        return !matched || best_allow;
    }

}  // namespace mc