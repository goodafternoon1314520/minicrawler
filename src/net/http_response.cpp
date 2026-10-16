#include "./http_response.h"

#include <algorithm>
#include <cctype>

namespace mc {
    namespace {
        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string trim (const std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return "";
            size_t e = s.find_last_not_of(" \t");
            return s.substr(b, e - b + 1);
        }

        // chunked格式：<十六进制长度>\r\n<数据>\r\n...0\r\n\r\n
        std::optional<std::string> dechunk(const std::string& in) {
            std::string out;
            size_t pos = 0;
            while (true) {
                size_t eol = in.find("\r\n", pos);
                if (eol == std::string::npos)
                    return std::nullopt;

                std::string size_str = in.substr(pos, eol - pos);
                if (auto sc = size_str.find(';'); sc != std::string::npos)
                    size_str.erase(sc);     //忽略chunk扩展
                size_t size = 0;
                try {
                    size = std::stoul(size_str, nullptr, 16);
                }
                catch (...) {
                    return std::nullopt;
                }

                pos = eol + 2;
                if (size == 0) return out;      //最后一个块
                if (pos + size > in.size())
                    return std::nullopt;
                out.append(in, pos, size);
                pos += size;
                if (in.compare(pos, 2, "\r\n") != 0)
                    return std::nullopt;
                pos += 2;
            }
        }
    }

    std::optional<HttpResponse> parse_http_response(const std::string& raw) {
        // 头和体以空行(\r\n\r\n)分隔
        size_t head_end = raw.find("\r\n\r\n");
        if (head_end == std::string::npos) return std::nullopt;
        std::string head = raw.substr(0, head_end);
        std::string body = raw.substr(head_end + 4);

        // 状态行：HTTP/1.1 200 OK
        size_t line_end = head.find("\r\n");
        std::string status_line = head.substr(0, line_end);
        if (status_line.rfind("HTTP/", 0) != 0) return std::nullopt;

        size_t sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) return std::nullopt;
        size_t sp2 = status_line.find(' ', sp1 + 1);
        std::string code = status_line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
        if (code.size() != 3 || !std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::nullopt;

        HttpResponse resp;
        resp.status = std::stoi(code);
        if (sp2 != std::string::npos)
            resp.reason = status_line.substr(sp2 + 1);

        // 头部：每行"Key: Value"
        size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = (e == std::string::npos) ? head.size() : e + 2;

            size_t c = line.find(':');
            if (c == std::string::npos) return std::nullopt;
            resp.headers[lower(line.substr(0, c))] = trim(line.substr(c + 1));
        }

        // 处理消息体
        auto te = resp.headers.find("transfer-encoding");
        if (te != resp.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
            auto d = dechunk(body);
            if (!d) return std::nullopt;
            body = std::move(*d);
        }
        else if (auto cl = resp.headers.find("content-length"); cl != resp.headers.end()) {
            try {
                size_t n = std::stoul(cl -> second);
                if (body.size() > n)
                    body.resize(n);
            }
            catch (...) {
                return std::nullopt;
            }
        }
        resp.body = std::move(body);
        return resp;
    }
}