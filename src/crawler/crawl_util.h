// 并发爬虫
#pragma once
#include <algorithm>
#include <cctype>
#include <string>

#include "net/http_response.h"

namespace mc {
    inline bool is_redirect(int s) {
        return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
    }

    inline bool is_html(const HttpResponse& r) {
        auto it = r.headers.find("content-type");
        if (it == r.headers.end())
            return false;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        return v.find("text/html") != std::string::npos;
    }
}