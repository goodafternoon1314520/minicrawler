#pragma once

#include <cstdint>
#include <string>

#include "../crawler/frontier.h"
#include "../crawler/page_sink.h"
#include "./mysql_db.h"

namespace mc {
    // 建表
    bool ensure_schema(Db& db, std::string* err);

    struct TaskCounts {
        uint64_t pending = 0, in_progress = 0, done = 0, failed = 0;
    };

    // 持久化的任务队列
    class MysqlFrontier : public Frontier {
    public:
        explicit MysqlFrontier(Db& db) : db_(db) {}

        // 把上次崩溃遗留的in_progress任务改回pending，返回恢复的数量
        size_t recover();
        TaskCounts counts();

        bool push(const Url& url, int depth) override;
        void mark_seen(const Url& url, int depth) override;
        std::optional<Task> pop() override;
        void complete(const Url& url, bool ok, const std::string& error) override;

    private:
        Db& db_;
    };

    class MysqlPageSink : public PageSink {
    public:
        explicit MysqlPageSink(Db& db) : db_(db) {}
        bool save(const PageRecord& page, std::string* err) override;

    private:
        Db& db_;
    };
}