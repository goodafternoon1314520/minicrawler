#include "redis/redis_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace mc {
    namespace {
        constexpr size_t kMaxReply = 64 * 1024 * 1024;
        std::string env_or(const char* key, const char* def) {
            const char* v = std::getenv(key);
            return v ? v : def;
        }
    }

    RedisConfig RedisConfig::from_env() {
        RedisConfig c;
        c.host = env_or("MC_REDIS_HOST", "127.0.0.1");
        c.port = static_cast<unsigned>(std::strtoul(env_or("MC_REDIS_PORT", "6379").c_str(), nullptr, 10));
        c.password = env_or("MC_REDIS_PASS", "");
        return c;
    }

    bool RedisClient::connect(const RedisConfig& cfg, std::string* err) {
        cfg_ = cfg;
        configured_ = true;
        return open(err);
    }

    bool RedisClient::open(std::string* err) {
        if (!configured_) {
            *err = "redis client not configured";
            return false;
        }
        close();

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        int rc = ::getaddrinfo(cfg_.host.c_str(), std::to_string(cfg_.port).c_str(), &hints, &res);
        if (rc != 0) {
            *err = std::string("getaddrinfo: ") + ::gai_strerror(rc);
            return false;
        }
        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(res, ::freeaddrinfo);

        int last_errno = 0;
        for (addrinfo* p = res; p; p = p->ai_next) {
            Fd s(::socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol));
            if (!s) {
                last_errno = errno;
                continue;
            }
            timeval tv(cfg_.timeout_sec, 0);
            ::setsockopt(s.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            ::setsockopt(s.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            // Redis命令都是小请求-小回复，Nagle算法会把小包攒一攒再发，白白增加延迟
            int one = 1;
            ::setsockopt(s.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            if (::connect(s.get(), p->ai_addr, p->ai_addrlen) == 0) {
                fd_ = std::move(s);
                break;
            }
            last_errno = errno;
        }
        if (!fd_) {
            *err = std::string("connect: ") + std::strerror(last_errno);
            return false;
        }

        if (!cfg_.password.empty()) {
            RespValue r;
            if (!roundtrip({"AUTH", cfg_.password}, &r, err))
                return false;
            if (r.type == RespValue::Type::kError) {
                *err = "redis: " + r.str;
                close();
                return false;
            }
        }
        return true;
    }

    bool RedisClient::roundtrip(const std::vector<std::string> &args, mc::RespValue *out, std::string *err) {
        const std::string wire = encode_command(args);
        size_t off = 0;
        while (off < wire.size()) {
            ssize_t n = ::send(fd_.get(), wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            *err = std::string("seed: ") + (n < 0 ? std::strerror(errno) : "connection closed");
            close();                    // 连接状态不明，丢掉，下次重连
            return false;
        }
        return read_reply(out, err);
    }

    bool RedisClient::read_reply(mc::RespValue *out, std::string *err) {
        char buf[8192];
        while (true) {
            // 注意：每次recv后都从头解析一遍缓冲区，对这种小回复没问题
            // 回复很大时是O(n^2)，那时应该做增量解析
            ParseResult r = parse_resp(rbuf_);
            if (r.status == ParseStatus::kOk) {
                rbuf_.erase(0, r.consumed);
                *out = std::move(r.value);
                return true;
            }
            if (r.status == ParseStatus::kError) {
                *err = "protocol error: malformed reply";
                close();
                return false;
            }
            ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
            if (n > 0) {
                rbuf_.append(buf, static_cast<size_t>(n));
                if (rbuf_.size() > kMaxReply) {
                    *err = "reply too large";
                    return false;
                }
                continue;
            }
            if (n == 0) {
                *err = "connection closed by server";
                close();
                return false;
            }
            if (errno == EINTR)
                continue;
            *err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "timeout" : std::string("recv: ") + std::strerror(errno);

            close();
            return false;
        }
    }

    bool RedisClient::command(const std::vector<std::string> &args, mc::RespValue *out, std::string *err) {
        std::string local_err;
        RespValue local_out;
        if (!err)
            err = &local_err;
        if (!out)
            out = &local_out;

        if (!fd_ && !open(err))
            return false;
        if (!roundtrip(args, out, err))
            return false;
        if (out->type == RespValue::Type::kError) {
            *err = "redis: " + out->str;
            return false;
        }
        return true;
    }

    bool RedisClient::eval(const std::string& script, const std::vector<std::string> &keys,
                           const std::vector<std::string> &args, mc::RespValue *out, std::string *err) {
        std::vector<std::string> cmd;
        cmd.reserve(3 + keys.size() + args.size());
        cmd.push_back("EVAL");
        cmd.push_back(script);
        cmd.push_back(std::to_string(keys.size()));
        for (const auto& k : keys)
            cmd.push_back(k);
        for (const auto& a : args)
            cmd.push_back(a);
        return command(cmd, out, err);
    }
}