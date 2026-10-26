#pragma once
#include <string>

#include "util/url.h"

namespace mc {
    inline std::string build_http_get_request(const Url& url) {
        std::string target = url.path;
        if (!url.query.empty())
            target += "?" + url.query;
        std::string host = url.host;
        if (url.port != 80)
            host += ":" + std::to_string(url.port);

        std::string req;
        req += "GET " + target + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "User-Agent: minicrawler/0.1\r\n";
        req += "Accept-Encoding: identity\r\n";
        req += "Connection: close\r\n";
        req += "\r\n";
        return req;
    }
}