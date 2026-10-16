#pragma once
#include <optional>
#include <string>

#include "./http_response.h"
#include "../util/url.h"

namespace mc {
    // 阻塞式HTTP GET，失败返回nullopt，原因写入*error
    std::optional<HttpResponse> http_get(const Url& url, std::string* error = nullptr, int timeout_sec = 10);
}