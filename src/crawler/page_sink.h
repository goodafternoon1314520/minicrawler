#pragma once

#include <string>

namespace mc {
    struct PageRecord {
        std::string url;        // 最终URL
        int depth = 0;
        int status = 0;
        std::string content_type;
        std::string body;       // 原始字节
    };

    class PageSink {
    public:
        virtual ~PageSink() = default;
        virtual bool save(const PageRecord& page, std::string* err) = 0;
    };
}