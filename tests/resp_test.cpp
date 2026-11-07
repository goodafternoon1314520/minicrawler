#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "redis/resp.h"

using mc::ParseStatus;
using mc::parse_resp;
using mc::RespValue;
using T = mc::RespValue::Type;

TEST(RespTest, SimpleErrorInteger) {
    auto s = parse_resp("+OK\r\n");
    ASSERT_EQ(s.status, ParseStatus::kOk);
    EXPECT_EQ(s.value.type, T::kSimple);
    EXPECT_EQ(s.value.str, "OK");
    EXPECT_EQ(s.consumed, 5u);

    auto e = parse_resp("-ERR unknown command\r\n");
    ASSERT_EQ(e.status, ParseStatus::kOk);
    EXPECT_EQ(e.value.type, T::kError);
    EXPECT_EQ(e.value.str, "ERR unknown command");

    auto i = parse_resp(":-42\r\n");
    ASSERT_EQ(i.status, ParseStatus::kOk);
    EXPECT_EQ(i.value.integer, -42);
}

TEST(RespTest, BulkIsBinarySafe) {
    // 数据里含 \r\n：必须按长度取，不能找结束符
    auto a = parse_resp(std::string("$8\r\nab\r\ncd\r\n\r\n"));
    ASSERT_EQ(a.status, ParseStatus::kOk);
    EXPECT_EQ(a.value.str, "ab\r\ncd\r\n");

    // 含 NUL 和 0xFF
    auto b = parse_resp(std::string("$3\r\n\0a\xff\r\n", 9));
    ASSERT_EQ(b.status, ParseStatus::kOk);
    EXPECT_EQ(b.value.str, std::string("\0a\xff", 3));

    auto empty = parse_resp("$0\r\n\r\n");
    ASSERT_EQ(empty.status, ParseStatus::kOk);
    EXPECT_EQ(empty.value.type, T::kBulk);
    EXPECT_EQ(empty.value.str, "");
}

TEST(RespTest, NullsAndArrays) {
    EXPECT_EQ(parse_resp("$-1\r\n").value.type, T::kNull);
    EXPECT_EQ(parse_resp("*-1\r\n").value.type, T::kNull);

    auto empty = parse_resp("*0\r\n");
    ASSERT_EQ(empty.status, ParseStatus::kOk);
    EXPECT_EQ(empty.value.type, T::kArray);
    EXPECT_TRUE(empty.value.array.empty());

    auto a = parse_resp("*2\r\n$3\r\nfoo\r\n:42\r\n");
    ASSERT_EQ(a.status, ParseStatus::kOk);
    ASSERT_EQ(a.value.array.size(), 2u);
    EXPECT_EQ(a.value.array[0].str, "foo");
    EXPECT_EQ(a.value.array[1].integer, 42);

    auto nested = parse_resp("*2\r\n*1\r\n+x\r\n$-1\r\n");
    ASSERT_EQ(nested.status, ParseStatus::kOk);
    EXPECT_EQ(nested.value.array[0].array[0].str, "x");
    EXPECT_EQ(nested.value.array[1].type, T::kNull);
}

TEST(RespTest, EveryProperPrefixIsIncomplete) {
    const std::string full = "*3\r\n$3\r\nfoo\r\n:42\r\n*1\r\n+x\r\n";
    for (size_t i = 0; i < full.size(); ++i)
        EXPECT_EQ(parse_resp(std::string_view(full).substr(0, i)).status, ParseStatus::kIncomplete) << i;
    auto r = parse_resp(full);
    EXPECT_EQ(r.status, ParseStatus::kOk);
    EXPECT_EQ(r.consumed, full.size());
}

TEST(RespTest, PipelinedRepliesConsumeOnlyTheFirst) {
    const std::string two = "+A\r\n+B\r\n";
    auto r = parse_resp(two);
    ASSERT_EQ(r.status, ParseStatus::kOk);
    EXPECT_EQ(r.value.str, "A");
    EXPECT_EQ(r.consumed, 4u);
    auto r2 = parse_resp(std::string_view(two).substr(r.consumed));
    EXPECT_EQ(r2.value.str, "B");
}

TEST(RespTest, MalformedInput) {
    EXPECT_EQ(parse_resp("?x\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_resp(":abc\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_resp("$-5\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_resp("*-5\r\n").status, ParseStatus::kError);
    EXPECT_EQ(parse_resp("$3\r\nfooXX").status, ParseStatus::kError);          // 结尾不是 \r\n
    EXPECT_EQ(parse_resp("$999999999999\r\n").status, ParseStatus::kError);   // 长度超限
}

TEST(RespTest, EncodeCommand) {
    EXPECT_EQ(mc::encode_command({"SET", "k", "v"}), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
    EXPECT_EQ(mc::encode_command({"PING"}), "*1\r\n$4\r\nPING\r\n");
    const std::string bin("a\0b", 3);
    EXPECT_EQ(mc::encode_command({"X", bin}), std::string("*2\r\n$1\r\nX\r\n$3\r\na\0b\r\n", 20));
}