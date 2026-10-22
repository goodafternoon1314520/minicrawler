#include "storage/mysql_store.h"

#include <algorithm>
#include <iostream>

namespace mc {
    namespace {

        constexpr int64_t kPending = 0, kInProgress = 1, kDone = 2, kFailed = 3;

// url 列用 ascii 字符集，所以入库前要求 URL 只含可见 ASCII。
// 真实网页里的非 ASCII 链接通常已经是 %E4%B8%AD 这种形式；个别没编码的会被跳过。
        bool storable_url(const std::string& s) {
            if (s.empty() || s.size() > 2048) return false;
            return std::all_of(s.begin(), s.end(),
                               [](unsigned char c) { return c >= 0x21 && c <= 0x7E; });
        }

// 把任意字节串变成可见 ASCII 并截断，用于 content_type / error 这类"别人给的字符串"
        std::string sanitize_ascii(const std::string& s, size_t max_len) {
            std::string out;
            for (unsigned char c : s) {
                if (out.size() >= max_len) break;
                out += (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
            }
            return out;
        }

        void log_err(const char* where, const std::string& err) {
            std::cerr << "[mysql] " << where << ": " << err << "\n";
        }

// url_hash 是唯一键：直接给 2048 字节的 URL 建唯一索引会超过 InnoDB 的索引长度上限，
// 所以对 URL 做 MD5，存 16 字节。哈希在 SQL 里算，不需要 C++ 端引入哈希库。
// 用 ON DUPLICATE KEY UPDATE id = id 而不是 INSERT IGNORE：
// 后者会把"数据太长/类型错误"这类真正的错误也悄悄吞掉。
        const char* kInsertTask =
                "INSERT INTO crawl_tasks (url_hash, url, depth, status) "
                "VALUES (UNHEX(MD5(?)), ?, ?, ?) ON DUPLICATE KEY UPDATE id = id";

        const char* kCreateTasks =
                "CREATE TABLE IF NOT EXISTS crawl_tasks ("
                "  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
                "  url_hash   BINARY(16) NOT NULL,"
                "  url        VARCHAR(2048) CHARACTER SET ascii NOT NULL,"
                "  depth      INT NOT NULL,"
                "  status     TINYINT NOT NULL DEFAULT 0,"
                "  last_error VARCHAR(255) CHARACTER SET ascii NOT NULL DEFAULT '',"
                "  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                "  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
                "  PRIMARY KEY (id),"
                "  UNIQUE KEY uk_url_hash (url_hash),"
                "  KEY idx_status_id (status, id)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

        const char* kCreatePages =
                "CREATE TABLE IF NOT EXISTS pages ("
                "  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
                "  url_hash     BINARY(16) NOT NULL,"
                "  url          VARCHAR(2048) CHARACTER SET ascii NOT NULL,"
                "  depth        INT NOT NULL,"
                "  status_code  INT NOT NULL,"
                "  content_type VARCHAR(255) CHARACTER SET ascii NOT NULL DEFAULT '',"
                "  body_size    INT UNSIGNED NOT NULL,"
                "  body         MEDIUMBLOB NOT NULL,"
                "  fetched_at   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                "  PRIMARY KEY (id),"
                "  UNIQUE KEY uk_url_hash (url_hash)"
                ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    }  // namespace

    bool ensure_schema(Db& db, std::string* err) {
        return db.exec(kCreateTasks, err) && db.exec(kCreatePages, err);
    }

// ---------------- MysqlFrontier ----------------

    bool MysqlFrontier::push(const Url& url, int depth) {
        const std::string s = url.to_string();
        if (!storable_url(s)) return false;
        uint64_t affected = 0;
        std::string err;
        if (!db_.execute(kInsertTask, {s, s, int64_t(depth), kPending}, &affected, &err)) {
            log_err("push", err);
            return false;
        }
        return affected == 1;  // 1=新插入；0=已存在
    }

    void MysqlFrontier::mark_seen(const Url& url, int depth) {
        const std::string s = url.to_string();
        if (!storable_url(s)) return;
        std::string err;
        if (!db_.execute(kInsertTask, {s, s, int64_t(depth), kDone}, nullptr, &err))
            log_err("mark_seen", err);
    }

    std::optional<Task> MysqlFrontier::pop() {
        std::string err;
        while (true) {
            // idx_status_id (status, id) 让这条查询直接定位到"最小的待抓 id"
            auto rows = db_.query(
                    "SELECT id, url, depth FROM crawl_tasks WHERE status = 0 ORDER BY id LIMIT 1", {},
                    &err);
            if (!rows) {
                log_err("pop", err);  // 数据库出错也当作"没有任务"，本次运行结束
                return std::nullopt;
            }
            if (rows->empty()) return std::nullopt;

            const Row& r = (*rows)[0];
            const int64_t id = static_cast<int64_t>(std::stoull(r.cols[0]));

            // 抢占：只有 status 仍是 0 才改成功。
            // 单进程下一定成功；将来多个爬虫进程并发时，失败的一方重新取下一个即可。
            uint64_t affected = 0;
            if (!db_.execute("UPDATE crawl_tasks SET status = 1 WHERE id = ? AND status = 0", {id},
                             &affected, &err)) {
                log_err("pop(claim)", err);
                return std::nullopt;
            }
            if (affected != 1) continue;

            auto url = parse_url(r.cols[1]);
            if (!url) {  // 理论上不会发生：入库的都是我们自己规范化过的 URL
                db_.execute("UPDATE crawl_tasks SET status = 3, last_error = 'unparsable url' "
                            "WHERE id = ?",
                            {id}, nullptr, &err);
                continue;
            }
            return Task{*url, std::stoi(r.cols[2])};
        }
    }

    void MysqlFrontier::complete(const Url& url, bool ok, const std::string& error) {
        std::string err;
        if (!db_.execute("UPDATE crawl_tasks SET status = ?, last_error = ? "
                         "WHERE url_hash = UNHEX(MD5(?))",
                         {int64_t(ok ? kDone : kFailed), sanitize_ascii(error, 255), url.to_string()},
                         nullptr, &err))
            log_err("complete", err);
    }

    size_t MysqlFrontier::recover() {
        uint64_t n = 0;
        std::string err;
        if (!db_.execute("UPDATE crawl_tasks SET status = 0 WHERE status = 1", {}, &n, &err))
            log_err("recover", err);
        return static_cast<size_t>(n);
    }

    TaskCounts MysqlFrontier::counts() {
        TaskCounts c;
        std::string err;
        auto rows = db_.query("SELECT status, COUNT(*) FROM crawl_tasks GROUP BY status", {}, &err);
        if (!rows) {
            log_err("counts", err);
            return c;
        }
        for (const Row& r : *rows) {
            const int st = std::stoi(r.cols[0]);
            const uint64_t n = std::stoull(r.cols[1]);
            if (st == kPending) c.pending = n;
            else if (st == kInProgress) c.in_progress = n;
            else if (st == kDone) c.done = n;
            else if (st == kFailed) c.failed = n;
        }
        return c;
    }

// ---------------- MysqlPageSink ----------------

    bool MysqlPageSink::save(const PageRecord& p, std::string* err) {
        if (!storable_url(p.url)) {
            if (err) *err = "url not storable";
            return false;
        }
        // 同一个 URL 再次抓取：更新而不是新增（幂等）。
        // 注：MySQL 8.0.20+ 对 VALUES() 给出弃用警告但仍可用；改用别名语法会不兼容 MariaDB。
        return db_.execute(
                "INSERT INTO pages (url_hash, url, depth, status_code, content_type, body_size, body) "
                "VALUES (UNHEX(MD5(?)), ?, ?, ?, ?, ?, ?) "
                "ON DUPLICATE KEY UPDATE depth = VALUES(depth), status_code = VALUES(status_code), "
                "content_type = VALUES(content_type), body_size = VALUES(body_size), "
                "body = VALUES(body), fetched_at = CURRENT_TIMESTAMP",
                {p.url, p.url, int64_t(p.depth), int64_t(p.status), sanitize_ascii(p.content_type, 255),
                 int64_t(p.body.size()), p.body},
                nullptr, err);
    }

}  // namespace mc