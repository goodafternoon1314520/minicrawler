//
// Created by davidcopperfield on 26-10-15.
//
#include "./url.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace mc {
    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {return std::tolower(c);});
        return s;
    }

    static std::string trim_ws(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // 处理路径里的"." 和 ".."，要求path以'/'开头
    static std::string remove_dot_segments(const std::string& path) {
        std::vector<std::string> out;
        bool dir_end = false;           // 结果以'/'结尾
        size_t pos = 1;
        while (true) {
            size_t next = path.find('/', pos);
            std::string s = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            if (s == "..") {
                if (!out.empty())
                    out.pop_back();         // 不能越过根目录
                dir_end = true;
            }
            else if (s == ".")
                dir_end = true;
            else {
                out.push_back(s);
                dir_end = false;
            }
            if (next == std::string::npos)
                break;
            pos = next + 1;
        }
        std::string r = "/";
        for (size_t i = 0; i < out.size(); ++i) {
            if (i)
                r += '/';
            r += out[i];
        }
        if (dir_end && !out.empty())
            r += '/';

        return r;
    }

    std::optional<Url> parse_url(const std::string& raw) {
        Url u;

        // 1.scheme
        auto pos = raw.find("://");
        if (pos == std::string::npos) return std::nullopt;
        u.scheme = to_lower(raw.substr(0, pos));
        if (u.scheme != "http" && u.scheme != "https") return std::nullopt;
        u.port = (u.scheme == "https") ? 443 : 80;

        // 2.去掉 #fragment（不会发给服务器）
        std::string rest = raw.substr(pos + 3);
        if (auto h = rest.find('#'); h != std::string::npos) rest.erase(h);

        // 3.拆出 authority(host[:port]) 和 path?query
        std::string authority;
        std::string path_query;
        if (auto s = rest.find('/'); s == std::string::npos) {
            authority = rest;
            path_query = "/";
        } else {
            authority = rest.substr(0, s);
            path_query = rest.substr(s);
        }

        // 4.host 和 port
        if (auto c = authority.find(':'); c != std::string::npos) {
            u.host = to_lower(authority.substr(0, c));
            std::string port_str = authority.substr(c + 1);
            if (port_str.empty() || port_str.size() > 5) return std::nullopt;
            int port = 0;
            for (char ch: port_str) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
                port = port * 10 + (ch - '0');
            }
            if (port < 1 || port > 65535) return std::nullopt;
            u.port = port;
        } else {
            u.host = to_lower(authority);
        }
        if (u.host.empty()) return std::nullopt;

        // 5.path 和 query
        if (auto q = path_query.find('?'); q != std::string::npos) {
            u.path = path_query.substr(0, q);
            u.query = path_query.substr(q + 1);
        } else {
            u.path = path_query;
        }

        u.path = remove_dot_segments(u.path);
        return u;
    }

    std::optional<Url> resolve_url(const Url& base, const std::string& ref_in) {
        std::string ref = trim_ws(ref_in);
        if (ref.empty())
            return std::nullopt;

        // 情况1：自带scheme
        size_t colon = ref.find(":");
        size_t delim = ref.find_first_of("/?#");
        if (colon != std::string::npos && (delim == std::string::npos || colon < delim))
            return parse_url(ref);      // 只认http/https

        // 情况2：//host/path
        if (ref.compare(0, 2, "//") == 0)
            return parse_url(base.scheme + ":" + ref);

        // 去掉 #fragment
        if (auto h = ref.find('#'); h != std::string::npos)
            ref.erase(h);

        Url u;
        u.scheme = base.scheme;
        u.host = base.host;
        u.port = base.port;

        // 情况3：纯#top，指向当前页
        if (ref.empty()) {
            u.path = base.path;
            u.query = base.query;
            return u;
        }

        std::string path_query;
        if (ref[0] == '?')
            path_query = base.path + ref;           // ?page=2
        else if (ref[0] == '/')
            path_query = ref;                       // /abs/path
        else
            path_query = base.path.substr(0, base.path.rfind('/') + 1) + ref;   //相对当前目录

        if (auto q = path_query.find('?'); q != std::string::npos) {
            u.path = path_query.substr(0, q);
            u.query = path_query.substr(q + 1);
        }
        else
            u.path = path_query;

        u.path = remove_dot_segments(u.path);
        return u;
    }

    std::string Url::to_string() const {
        std::string s = scheme + "://" + host;
        bool default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
        if (!default_port) s += ":" + std::to_string(port);
        s += path;
        if (!query.empty()) s += "?" + query;
        return s;
    }
}