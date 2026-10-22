#include "storage/mysql_db.h"

#include <cstdlib>
#include <cstring>

namespace mc {
    namespace {

        struct StmtGuard {
            MYSQL_STMT* s;
            ~StmtGuard() {
                if (s) mysql_stmt_close(s);
            }
        };

        std::string env_or(const char* key, const char* def) {
            const char* v = std::getenv(key);
            return v ? v : def;
        }

    }  // namespace

    DbConfig DbConfig::from_env() {
        DbConfig c;
        c.host = env_or("MC_DB_HOST", "localhost");
        c.port = static_cast<unsigned>(std::strtoul(env_or("MC_DB_PORT", "3306").c_str(), nullptr, 10));
        c.user = env_or("MC_DB_USER", "");
        c.password = env_or("MC_DB_PASS", "");
        c.database = env_or("MC_DB_NAME", "minicrawler");
        return c;
    }

    Db::~Db() { close(); }

    void Db::close() {
        if (conn_) {
            mysql_close(conn_);
            conn_ = nullptr;
        }
    }

    bool Db::connect(const DbConfig& c, std::string* err) {
        close();
        conn_ = mysql_init(nullptr);
        if (!conn_) {
            if (err) *err = "mysql_init failed";
            return false;
        }
        unsigned connect_timeout = 5, io_timeout = 30;
        mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
        mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &io_timeout);
        mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &io_timeout);
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

        if (!mysql_real_connect(conn_, c.host.c_str(), c.user.c_str(), c.password.c_str(),
                                c.database.c_str(), c.port, nullptr, 0)) {
            if (err) *err = mysql_error(conn_);
            close();
            return false;
        }
        return true;
    }

    bool Db::exec(const std::string& sql, std::string* err) {
        if (!conn_) {
            if (err) *err = "not connected";
            return false;
        }
        if (mysql_real_query(conn_, sql.data(), sql.size()) != 0) {
            if (err) *err = mysql_error(conn_);
            return false;
        }
        if (MYSQL_RES* r = mysql_store_result(conn_)) mysql_free_result(r);
        return true;
    }

    bool Db::execute(const std::string& sql, const std::vector<Param>& params, uint64_t* affected,
                     std::string* err) {
        return run(sql, params, affected, nullptr, err);
    }

    std::optional<std::vector<Row>> Db::query(const std::string& sql, const std::vector<Param>& params,
                                              std::string* err) {
        std::vector<Row> rows;
        if (!run(sql, params, nullptr, &rows, err)) return std::nullopt;
        return rows;
    }

    bool Db::run(const std::string& sql, const std::vector<Param>& params, uint64_t* affected,
                 std::vector<Row>* rows, std::string* err) {
        if (!conn_) {
            if (err) *err = "not connected";
            return false;
        }
        MYSQL_STMT* stmt = mysql_stmt_init(conn_);
        if (!stmt) {
            if (err) *err = "mysql_stmt_init failed";
            return false;
        }
        StmtGuard guard{stmt};
        auto fail = [&](const char* what) {
            if (err) *err = std::string(what) + ": " + mysql_stmt_error(stmt);
            return false;
        };

        // 1. prepare：SQL 文本只在这里发给服务器一次，之后只传参数，用户数据永远不会被当成 SQL 解析
        if (mysql_stmt_prepare(stmt, sql.data(), sql.size()) != 0) return fail("prepare");
        if (mysql_stmt_param_count(stmt) != params.size()) {
            if (err) *err = "parameter count mismatch";
            return false;
        }

        // 2. 绑定参数
        std::vector<MYSQL_BIND> pb(params.size());
        std::vector<unsigned long> plens(params.size(), 0);
        if (!params.empty()) {
            std::memset(pb.data(), 0, sizeof(MYSQL_BIND) * pb.size());
            for (size_t i = 0; i < params.size(); ++i) {
                if (std::holds_alternative<int64_t>(params[i])) {
                    pb[i].buffer_type = MYSQL_TYPE_LONGLONG;
                    pb[i].buffer = const_cast<int64_t*>(&std::get<int64_t>(params[i]));
                } else {
                    const std::string& s = std::get<std::string>(params[i]);
                    plens[i] = s.size();
                    pb[i].buffer_type = MYSQL_TYPE_BLOB;  // 当作二进制传，不做字符集转换
                    pb[i].buffer = const_cast<char*>(s.data());
                    pb[i].buffer_length = s.size();
                    pb[i].length = &plens[i];
                }
            }
            if (mysql_stmt_bind_param(stmt, pb.data()) != 0) return fail("bind_param");
        }

        // 3. 执行
        if (mysql_stmt_execute(stmt) != 0) return fail("execute");
        if (affected) *affected = mysql_stmt_affected_rows(stmt);

        // 4. 读结果集（如果有）
        if (rows && mysql_stmt_field_count(stmt) > 0) {
            if (mysql_stmt_store_result(stmt) != 0) return fail("store_result");
            const unsigned ncols = mysql_stmt_field_count(stmt);

            // 先不给缓冲区，只让服务器告诉我们每列有多长，再按长度分配
            std::vector<MYSQL_BIND> rb(ncols);
            std::memset(rb.data(), 0, sizeof(MYSQL_BIND) * ncols);
            std::vector<unsigned long> lens(ncols, 0);
            for (unsigned i = 0; i < ncols; ++i) {
                rb[i].buffer_type = MYSQL_TYPE_STRING;
                rb[i].length = &lens[i];
            }
            if (mysql_stmt_bind_result(stmt, rb.data()) != 0) return fail("bind_result");

            while (true) {
                int rc = mysql_stmt_fetch(stmt);
                if (rc == MYSQL_NO_DATA) break;
                if (rc == 1) return fail("fetch");  // 0 和 MYSQL_DATA_TRUNCATED 都表示这一行可用

                Row row;
                row.cols.resize(ncols);
                for (unsigned i = 0; i < ncols; ++i) {
                    row.cols[i].resize(lens[i]);
                    if (lens[i] == 0) continue;
                    unsigned long out_len = 0;
                    MYSQL_BIND b;
                    std::memset(&b, 0, sizeof b);
                    b.buffer_type = MYSQL_TYPE_STRING;
                    b.buffer = &row.cols[i][0];
                    b.buffer_length = lens[i];
                    b.length = &out_len;
                    if (mysql_stmt_fetch_column(stmt, &b, i, 0) != 0) return fail("fetch_column");
                }
                rows->push_back(std::move(row));
            }
        }
        return true;
    }

}  // namespace mc