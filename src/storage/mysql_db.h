#pragma once
#include <mysql/mysql.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mc {
    struct DbConfig {
        std::string host = "localhost";      // "localhost" 走unix socket，"127.0.0.1"走TCP
        unsigned port = 3306;
        std::string user;
        std::string password;
        std::string database;

        // 读取MC_DB_HOST / MC_DB_PORT / MC_DB_PASS / MC_DB_NAME
        static DbConfig from_env();
    };

    using Param = std::variant<int64_t, std::string>;   // 整数，或按字节原样传输的字符串
    struct Row {
        std::vector<std::string> cols;      // 所有列都以字符串形式返回
    };

    class Db {
    public:
        Db() = default;
        ~Db();
        Db(const Db&) = delete;
        Db& operator=(const Db&) = delete;

        bool connect(const DbConfig& cfg, std::string* err);

        // 无参数，无用户输入的语句
        bool exec(const std::string& sql, std::string* err);

        // 预处理语句：用?占位，参数单独传递，affected返回受影响行数
        bool execute(const std::string& sql, const std::vector<Param>& params = {}, uint64_t* affected = nullptr, std::string* err = nullptr);

        // 预处理语句查询
        std::optional<std::vector<Row>> query(const std::string& sql, const std::vector<Param>& params = {}, std::string* err = nullptr);

    private:
        bool run(const std::string& sql, const std::vector<Param>& params, uint64_t* affected, std::vector<Row>* rows, std::string* err);
        void close();

        MYSQL* conn_ = nullptr;
    };
}