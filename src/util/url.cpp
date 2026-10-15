//
// Created by davidcopperfield on 26-10-15.
//
#include "./url.h"

#include <algorithm>
#include <cctype>

namespace mc {
    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {return std::tolower(c);});
        return s;
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