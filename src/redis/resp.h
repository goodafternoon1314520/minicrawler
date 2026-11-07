#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc {
    struct RespValue {
        enum class Type { kNull, kSimple, kError, kInteger, kBulk, kArray };
        Type type = Type::kNull;
        std::string str;                // kSimple / kError / kBulk
        int64_t integer = 0;            // kInteger
        std::vector<RespValue> array;   // kArray
    };

    enum class ParseStatus { kOk, kIncomplete, kError };

    struct ParseResult {
        ParseStatus status = ParseStatus::kIncomplete;
        size_t consumed = 0;             // 仅kOk有效，用掉了多少字节
        RespValue value;
    };

    // 从缓冲区开头解析一个完整的值，数据不够返回kIncomplete（调用方继续recv再试）
    // 数据非法返回kError，TCP是字节流，一次recv可能只收到半个回复，也可能收到好几个
    ParseResult parse_resp(std::string_view in);

    // 把命令编码成线上格式
    std::string encode_command(const std::vector<std::string>& args);
}