//
// Created by davidcopperfield on 26-10-15.
//

#pragma once

#include <optional>
#include <string>

namespace mc {
    struct Url {
        std::string scheme;     // "http" / "https"
        std::string host;       // "example.com"
        int port = 80;          // 默认端口根据scheme决定
        std::string path;       // "/a/b", 至少是"/"
        std::string query;      // "x=1&y=2", 不含"?"

        // 重新拼成字符串，URL去重
        std::string to_string() const;
    };

    // 解析失败返回std::nullopt，不抛异常
    // 爬虫会遇到大量脏数据，失败是常态
    std::optional<Url> parse_url(const std::string& raw);
}