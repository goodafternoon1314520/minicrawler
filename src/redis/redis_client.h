#pragma once
#include <string>
#include <vector>

#include "net/fd.h"
#include "redis/resp.h"

namespace mc {
    struct RedisConfig {
        std::string host = "127.0.0.1";
        unsigned port = 6379;
        std::string password;               // 空 = 不认证
        int timeout_sec = 5;

        // 读取MC_REDIS_HOST / MC_REDIS_PORT / MC_REDIS_PASS
        static RedisConfig from_env();
    };

    // 阻塞式，单连接，非线程安全，同一时刻只能被一个线程使用
    class RedisClient {
    public:
        bool connect(const RedisConfig& cfg, std::string* err);
        bool connected() const { return static_cast<bool>(fd_); }

        // 发一条命令，等一条回复
        // 返回false：网络/协议错误，或Redis回了-ERR（err里是原因，out里仍有回复内容
        // 连接断开后，下一次调用会自动连接一次，但断开前发出的那条命令不会重发
        // 因为我们无法得知他是否已经执行过
        // 小心：std::vector<std::string>{"GET", "k"}两个字符串字面量会被方程迭代器区间
        // 构造函数处理，结果是未定义行为，两个元素时至少让其中一个是std::string
        bool command(const std::vector<std::string>& args, RespValue* out, std::string* err);

        bool eval(const std::string& script, const std::vector<std::string>& keys, const std::vector<std::string>& args, RespValue* out, std::string* err);

    private:
        bool open(std::string* err);
        bool roundtrip(const std::vector<std::string>& args, RespValue* out, std::string* err);
        bool read_reply(RespValue* out, std::string* err);
        void close() {
            fd_.reset();
            rbuf_.clear();
        }

        RedisConfig cfg_;
        bool configured_ = false;
        Fd fd_;
        std::string rbuf_;              // 已收到但还没解析的字节
    };
}