#include "redis/resp.h"

#include <algorithm>
#include <charconv>

namespace mc {
    namespace {
        constexpr int64_t kMaxBulk = 512LL * 1024 * 1024;           // Redis自己的上限
        constexpr int64_t kMaxArray = 1000000;
        constexpr int kMaxDepth = 16;

        bool read_line(std::string_view in, size_t pos, std::string_view* line, size_t* next) {
            size_t e = in.find("\r\n", pos);
            if (e == std::string_view::npos)
                return false;
            *line = in.substr(pos, e - pos);
            *next = e + 2;
            return true;
        }

        bool to_int(std::string_view s, int64_t* out) {
            if (s.empty())
                return false;
            int64_t v = 0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            if (ec != std::errc() || p != s.data() + s.size())
                return false;
            *out = v;
            return true;
        }

        // 成功时把*pos推进到这个值的末尾，失败/不完整时*pos不保证
        ParseStatus parse_at(std::string_view in, size_t* pos, RespValue* out, int depth) {
            if (depth > kMaxDepth)
                return ParseStatus::kError;
            if (*pos >= in.size())
                return ParseStatus::kIncomplete;

            const char type = in[*pos];
            std::string_view line;
            size_t next = 0;
            if (!read_line(in, *pos + 1, &line, &next))
                return ParseStatus::kIncomplete;

            switch (type) {
                case '+':
                    out->type = RespValue::Type::kSimple;
                    out->str.assign(line.data(), line.size());
                    *pos = next;
                    return ParseStatus::kOk;

                case '-':
                    out->type = RespValue::Type::kError;
                    out->str.assign(line.data(), line.size());
                    *pos = next;
                    return ParseStatus::kOk;

                case ':': {
                    int64_t v = 0;
                    if (!to_int(line, &v))
                        return ParseStatus::kError;
                    out->type = RespValue::Type::kInteger;
                    out->integer = v;
                    *pos = next;
                    return ParseStatus::kOk;
                }

                case '$': {
                    int64_t n = 0;
                    if (!to_int(line, &n))
                        return ParseStatus::kError;
                    if (n == -1) {
                        out->type = RespValue::Type::kNull;
                        *pos = next;
                        return ParseStatus::kOk;
                    }
                    if (n < 0 || n > kMaxBulk)
                        return ParseStatus::kError;
                    const size_t len = static_cast<size_t>(n);
                    if (in.size() - next < len + 2)
                        return ParseStatus::kIncomplete;        // 数据 + \r\n还没到齐
                    if (in[next + len] != '\r' || in[next + len + 1] != '\n')
                        return ParseStatus::kError;
                    out->type = RespValue::Type::kBulk;
                    out->str.assign(in.data() + next, len); // 按长度取，数据里可以含\r\n 和 \0
                    *pos = next + len + 2;
                    return ParseStatus::kOk;
                }

                case '*': {
                    int64_t n = 0;
                    if (!to_int(line, &n))
                        return ParseStatus::kError;
                    if (n == -1) {
                        out->type = RespValue::Type::kNull;
                        *pos = next;
                        return ParseStatus::kOk;
                    }
                    if (n < 0 || n > kMaxArray)
                        return ParseStatus::kError;
                    out->type = RespValue::Type::kArray;
                    out->array.clear();
                    out->array.reserve(static_cast<size_t>(std::min<int64_t>(n, 1024)));
                    size_t p = next;
                    for (int64_t i = 0; i < n; ++i) {
                        RespValue v;
                        ParseStatus st = parse_at(in, &p, &v, depth + 1);
                        if (st != ParseStatus::kOk)
                            return st;
                        out->array.push_back(std::move(v));
                    }
                    *pos = p;
                    return ParseStatus::kOk;
                }
                default:
                    return ParseStatus::kError;
            }
        }
    }
    ParseResult parse_resp(std::string_view in) {
        ParseResult r;
        size_t pos = 0;
        r.status = parse_at(in, &pos, &r.value, 0);
        if (r.status == ParseStatus::kOk)
            r.consumed = pos;
        return r;
    }

    std::string encode_command(const std::vector<std::string>& args) {
        std::string out = "*" + std::to_string(args.size()) + "\r\n";
        for (const auto& a : args) {
            out += "$" + std::to_string(a.size()) + "\r\n";
            out += a;
            out += "\r\n";
        }
        return out;
    }
}