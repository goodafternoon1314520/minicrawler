// 并发爬虫
#pragma once
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

#include "net/http_response.h"

namespace mc {

    inline bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

    inline bool is_html(const HttpResponse& r) {
        auto it = r.headers.find("content-type");
        if (it == r.headers.end()) return false;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        return v.find("text/html") != std::string::npos;
    }

// 这些状态码表示"服务器现在处理不了，稍后再试"。404/403/410 是"页面就是这样"，重试没有意义。
    inline bool is_retryable_status(int s) {
        return s == 408 || s == 425 || s == 429 || s == 500 || s == 502 || s == 503 || s == 504;
    }

// 网络层错误是否值得重试（err 是 AsyncHttpClient 给出的原因文本）
    inline bool is_retryable_error(const std::string& err) {
        if (err.find("too large") != std::string::npos) return false;  // 重试只会再收到同样大的响应
        if (err.find("only http") != std::string::npos) return false;  // 我们自己不支持，不是对方的问题
        if (err.rfind("getaddrinfo:", 0) == 0)                         // 域名不存在是永久的，DNS 暂时不可用才重试
            return err.find("Temporary") != std::string::npos;
        return true;  // timeout / connect refused / reset / 响应被截断 ...
    }

// Retry-After 只支持"秒数"写法（HTTP-date 写法忽略）。返回毫秒。
    inline std::optional<long> retry_after_ms(const HttpResponse& r) {
        auto it = r.headers.find("retry-after");
        if (it == r.headers.end()) return std::nullopt;
        const std::string& v = it->second;
        if (v.empty() || v.size() > 7 ||
            !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::nullopt;
        return std::stol(v) * 1000L;
    }

}  // namespace mc