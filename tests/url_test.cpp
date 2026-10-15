//
// Created by davidcopperfield on 26-10-15.
//
#include <gtest/gtest.h>

#include "util/url.h"

using mc::parse_url;

TEST(UrlTest, BasicHttp) {
    auto u = parse_url("http://Example.com/a/b?x=1#frag");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->scheme, "http");
    EXPECT_EQ(u->host, "example.com");  // 转小写
    EXPECT_EQ(u->port, 80);
    EXPECT_EQ(u->path, "/a/b");
    EXPECT_EQ(u->query, "x=1");          // fragment 被丢弃
}

TEST(UrlTest, DefaultAndPorts) {
    EXPECT_EQ(parse_url("https://a.com")->port, 443);
    EXPECT_EQ(parse_url("https://a.com")->path, "/");
    EXPECT_EQ(parse_url("https://a.com:8080/x")->port, 8080);
}

TEST(UrlTest, RejectsBadInput){
    EXPECT_FALSE(parse_url("ftp://a.com"));
    EXPECT_FALSE(parse_url("a.com/x"));
    EXPECT_FALSE(parse_url("http://"));
    EXPECT_FALSE(parse_url("http://a.com:abc/"));
    EXPECT_FALSE(parse_url("http://a.com:99999/"));
}

TEST(UrlTest, ToStringRoundTrip) {
    auto u = parse_url("HTTP://A.com:80/p?q=1#z");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->to_string(), "http://a.com/p?q=1");
}