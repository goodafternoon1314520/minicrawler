#pragma once
#include <map>
#include <optional>
#include <string>

namespace mc {
    struct HttpResponse {
        int status = 0;
        std::string reason;
        std::map<std::string, std::string> headers;     //key统一小写
        std::string body;
    };

    // 输入完整的原始相应字节流，解析失败返回nullopt
    std::optional<HttpResponse> parse_http_response(const std::string& raw);
}