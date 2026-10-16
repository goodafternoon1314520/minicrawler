#include <gtest/gtest.h>

#include "../src/net/http_response.h"

using mc::parse_http_response;

TEST(HttpResponseTest, ContentLength) {
    auto r = parse_http_response("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\nhello-extra");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->reason, "OK");
    EXPECT_EQ(r->headers["content-type"], "text/html");     //key小写
    EXPECT_EQ(r->body, "hello");                            //按Content-Length截断
}

TEST(HttpResponseTest, Chunked) {
    auto r = parse_http_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\nworld\r\n0\r\n\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->body, "hello world");
}

TEST(HttpResponseTest, NoReasonPhrase) {
    auto r = parse_http_response("Http/1.1 204\r\n\r\n");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 204);
    EXPECT_EQ(r->reason, "");
}

TEST(HttpResponseTest, Malformed) {
    EXPECT_FALSE(parse_http_response("garbage"));
    EXPECT_FALSE(parse_http_response("HTTP/1.1 abc OK\r\n\r\n"));
    EXPECT_FALSE(parse_http_response("HTTP/1.1 200 OK\r\nBadHeaderLine\r\n\r\n"));
    // chunked数据被截断
    EXPECT_FALSE(parse_http_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel"));
}