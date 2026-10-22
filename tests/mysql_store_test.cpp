#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "../src/storage/mysql_db.h"
#include "../src/storage/mysql_store.h"
#include "util/url.h"

namespace {
    mc::Url U(const char *s) { return *mc::parse_url(s); }

    class MysqlTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const char* name = std::getenv("MC_TEST_DB");
            if (!name)
                GTEST_SKIP() << "set MC_TEST_DB (e.g. minicrawler_test) to run MySQL tests";

            // 安全网：测试会TRUNCATE表，只允许在名字以_test结尾的库上运行
            std::string n = name;
            ASSERT_TRUE(n.size() > 5 && n.compare(n.size() - 5, 5, "_test") == 0) << "refusing to run on a non-test database: " << n;

            auto cfg = mc::DbConfig::from_env();
            cfg.database = n;
            std::string err;
            ASSERT_TRUE(db.connect(cfg, &err)) << err;
            ASSERT_TRUE(mc::ensure_schema(db, &err)) << err;
            ASSERT_TRUE(db.exec("TRUNCATE TABLE crawl_tasks", &err)) << err;
            ASSERT_TRUE(db.exec("TRUNCATE TABLE pages", &err)) << err;
        }
        mc::Db db;
    };
}

TEST_F(MysqlTest, SchemaIsIdempotent) {
    std::string err;
    EXPECT_TRUE(mc::ensure_schema(db, &err)) << err;
}

TEST_F(MysqlTest, FrontierDedupAndFifo) {
    mc::MysqlFrontier f(db);
    EXPECT_TRUE(f.push(U("http://a.com/1"), 0));
    EXPECT_TRUE(f.push(U("http://a.com/2"), 1));
    EXPECT_FALSE(f.push(U("http://a.com/1"), 5));  // 重复

    auto t1 = f.pop();
    ASSERT_TRUE(t1);
    EXPECT_EQ(t1->url.to_string(), "http://a.com/1");
    EXPECT_EQ(t1->depth, 0);
    auto t2 = f.pop();
    ASSERT_TRUE(t2);
    EXPECT_EQ(t2->url.to_string(), "http://a.com/2");
    EXPECT_EQ(t2->depth, 1);
    EXPECT_FALSE(f.pop());

    f.complete(t1->url, true, "");
    f.complete(t2->url, false, "boom");
    auto c = f.counts();
    EXPECT_EQ(c.done, 1u);
    EXPECT_EQ(c.failed, 1u);
    EXPECT_EQ(c.pending, 0u);
    EXPECT_EQ(c.in_progress, 0u);
}

TEST_F(MysqlTest, RecoverInterruptedTasks) {
    mc::MysqlFrontier f(db);
    f.push(U("http://a.com/1"), 0);
    ASSERT_TRUE(f.pop());       // 取走但不 complete，模拟进程崩溃
    EXPECT_FALSE(f.pop());

    mc::MysqlFrontier restarted(db);  // "重启"后的新对象
    EXPECT_EQ(restarted.recover(), 1u);
    auto t = restarted.pop();
    ASSERT_TRUE(t);
    EXPECT_EQ(t->url.to_string(), "http://a.com/1");
}

TEST_F(MysqlTest, MarkSeenPreventsEnqueue) {
    mc::MysqlFrontier f(db);
    f.mark_seen(U("http://a.com/redirected"), 1);
    EXPECT_FALSE(f.push(U("http://a.com/redirected"), 1));
    EXPECT_FALSE(f.pop());
}

TEST_F(MysqlTest, QuotesInUrlAreHarmless) {
    mc::MysqlFrontier f(db);
    const char* nasty = "http://a.com/?q='%20OR%20'1'='1";
    EXPECT_TRUE(f.push(U(nasty), 0));

    std::string err;
    auto rows = db.query("SELECT COUNT(*) FROM crawl_tasks", {}, &err);
    ASSERT_TRUE(rows) << err;
    EXPECT_EQ((*rows)[0].cols[0], "1");

    auto t = f.pop();
    ASSERT_TRUE(t);
    EXPECT_EQ(t->url.to_string(), nasty);  // 原样存取，没有被当成 SQL
}

TEST_F(MysqlTest, PageUpsertIsIdempotentAndBinarySafe) {
    mc::MysqlPageSink sink(db);
    std::string err;

    mc::PageRecord p{"http://a.com/x", 1, 200, "text/html", std::string("<h1>v1</h1>\0\xff", 13)};
    ASSERT_TRUE(sink.save(p, &err)) << err;

    auto r1 = db.query("SELECT COUNT(*), MAX(LENGTH(body)) FROM pages", {}, &err);
    ASSERT_TRUE(r1) << err;
    EXPECT_EQ((*r1)[0].cols[0], "1");
    EXPECT_EQ((*r1)[0].cols[1], "13");  // NUL 和 0xFF 都原样保存了

    p.status = 404;
    p.body = "gone";
    ASSERT_TRUE(sink.save(p, &err)) << err;  // 同一个 URL：更新，不是新增

    auto r2 = db.query("SELECT COUNT(*), MAX(status_code), MAX(body_size) FROM pages", {}, &err);
    ASSERT_TRUE(r2) << err;
    EXPECT_EQ((*r2)[0].cols[0], "1");
    EXPECT_EQ((*r2)[0].cols[1], "404");
    EXPECT_EQ((*r2)[0].cols[2], "4");
}