#include "parser/html_links.h"

#include <algorithm>
#include <cctype>

namespace mc {
    namespace {

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// 不区分大小写的子串查找（HTML 标签名不区分大小写）
        size_t ifind(const std::string& s, const std::string& needle, size_t from) {
            if (from > s.size()) return std::string::npos;
            auto it = std::search(s.begin() + from, s.end(), needle.begin(), needle.end(),
                                  [](char a, char b) {
                                      return std::tolower(static_cast<unsigned char>(a)) ==
                                             std::tolower(static_cast<unsigned char>(b));
                                  });
            return it == s.end() ? std::string::npos : static_cast<size_t>(it - s.begin());
        }

        std::string clean_href(std::string v) {
            for (size_t pos = 0; (pos = v.find("&amp;", pos)) != std::string::npos; ++pos)
                v.replace(pos, 5, "&");
            size_t b = v.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return "";
            size_t e = v.find_last_not_of(" \t\r\n");
            return v.substr(b, e - b + 1);
        }

    }  // namespace

    std::vector<std::string> extract_links(const std::string& html) {
        std::vector<std::string> links;
        const size_t n = html.size();
        size_t i = 0;

        while (i < n) {
            size_t lt = html.find('<', i);
            if (lt == std::string::npos) break;

            // 注释：整段跳过
            if (html.compare(lt, 4, "<!--") == 0) {
                size_t end = html.find("-->", lt + 4);
                if (end == std::string::npos) break;
                i = end + 3;
                continue;
            }
            // 只处理 "<字母" 开头的开始标签；</x>、<!DOCTYPE>、<?xml 都跳过
            if (lt + 1 >= n || !std::isalpha(static_cast<unsigned char>(html[lt + 1]))) {
                i = lt + 1;
                continue;
            }

            // 标签名
            size_t p = lt + 1;
            size_t name_start = p;
            while (p < n && std::isalnum(static_cast<unsigned char>(html[p]))) ++p;
            std::string name = lower(html.substr(name_start, p - name_start));

            // 属性
            bool has_href = false;
            std::string href;
            while (p < n) {
                while (p < n && (is_space(html[p]) || html[p] == '/')) ++p;
                if (p >= n) break;
                if (html[p] == '>') { ++p; break; }

                size_t a = p;
                while (p < n && !is_space(html[p]) && html[p] != '=' && html[p] != '>' &&
                       html[p] != '/')
                    ++p;
                std::string attr = lower(html.substr(a, p - a));
                while (p < n && is_space(html[p])) ++p;

                std::string value;
                if (p < n && html[p] == '=') {
                    ++p;
                    while (p < n && is_space(html[p])) ++p;
                    if (p < n && (html[p] == '"' || html[p] == '\'')) {
                        char q = html[p++];
                        size_t e = html.find(q, p);
                        if (e == std::string::npos) { p = n; break; }  // 引号没闭合，放弃
                        value = html.substr(p, e - p);
                        p = e + 1;
                    } else {
                        size_t v = p;
                        while (p < n && !is_space(html[p]) && html[p] != '>') ++p;
                        value = html.substr(v, p - v);
                    }
                }
                if (name == "a" && attr == "href") { has_href = true; href = value; }
            }
            i = p;

            if (name == "a" && has_href) links.push_back(clean_href(href));

            // <script>/<style> 里的文本不是 HTML，直接跳到结束标签
            if (name == "script" || name == "style") {
                size_t close = ifind(html, "</" + name, i);
                if (close == std::string::npos) break;
                i = close;
            }
        }
        return links;
    }

}  // namespace mc