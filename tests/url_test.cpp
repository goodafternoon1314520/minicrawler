//
// Created by davidcopperfield on 26-10-15.
//
#include <gtest/gtest.h>

#include "util/url.h"

using mc::parse_url;
using mc::resolve_url;

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

static std::string R(const std::string& ref) {
    auto base = parse_url("http://a.com/x/y/z.html?q=1");
    auto u = resolve_url(*base, ref);
    return u ? u->to_string() : "<null>";
}

TEST(ResolverUrlTest, RelativeForms) {
    EXPECT_EQ(R("../p.html"), "http://a.com/x/p.html");
    EXPECT_EQ(R("./a"), "http://a.com/x/y/a");
    EXPECT_EQ(R("sub/page"), "http://a.com/x/y/sub/page");
    EXPECT_EQ(R("../"), "http://a.com/x/");
    EXPECT_EQ(R("/root"), "http://a.com/root");
    EXPECT_EQ(R("//cdn.com/y"), "http://cdn.com/y");
    EXPECT_EQ(R("?k=2"), "http://a.com/x/y/z.html?k=2");
    EXPECT_EQ(R("#top"), "http://a.com/x/y/z.html?q=1");
    EXPECT_EQ(R(" /p "), "http://a.com/p");
}

TEST(ResolverUrlTest, CannotEscpeRoot) {
    EXPECT_EQ(R("http://B.com/z"), "https://b.com/z");
    EXPECT_EQ(R("javascript:void(0)"), "<null>");
    EXPECT_EQ(R("mailto:a@b.c"), "<null>");
    EXPECT_EQ(R("  "), "<null>");
}

TEST(UrlTest, DotSegmentsNormalized) {
    EXPECT_EQ(parse_url("http://a.com/a/./b/../c")->path, "/a/c");
}