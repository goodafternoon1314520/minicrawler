#include <gtest/gtest.h>

#include "parser/html_links.h"

using mc::extract_links;
using Links = std::vector<std::string>;

TEST(HtmlLinksTest, QuotingAndCase) {
    auto v = extract_links(
            R"(<a href="/a">1</a> <a href='/b'>2</a> <a href=/c>3</a> <A HREF="/d">4</A>)"
            R"(<a class="btn" id=x href="/e" target=_blank>5</a> <a href = "/sp">6</a>)");
    EXPECT_EQ(v, (Links{"/a", "/b", "/c", "/d", "/e", "/sp"}));
}

TEST(HtmlLinksTest, IgnoresNonAnchorsAndNoHref) {
    auto v = extract_links(R"(<a>x</a><link href="/css"><abbr href="/z">q</abbr><a name="n">)");
    EXPECT_TRUE(v.empty());
}

TEST(HtmlLinksTest, IgnoresCommentsAndScripts) {
    auto v = extract_links(
            R"(<!-- <a href="/hidden"> -->)"
            R"(<script>var s = '<a href="/js">';</script>)"
            R"(<style>a::after{content:"<a href='/css'>"}</style>)"
            R"(<a href="/real">ok</a>)");
    EXPECT_EQ(v, (Links{"/real"}));
}

TEST(HtmlLinksTest, DecodesAmpersand) {
    auto v = extract_links(R"(<a href="/p?x=1&amp;y=2">)");
    EXPECT_EQ(v, (Links{"/p?x=1&y=2"}));
}

TEST(HtmlLinksTest, MalformedDoesNotHang) {
    EXPECT_TRUE(extract_links(R"(<a href="/x)").empty());   // 引号没闭合
    EXPECT_TRUE(extract_links("<a").empty());
    EXPECT_TRUE(extract_links("<<<>>> < a href=x").empty());
    EXPECT_TRUE(extract_links("").empty());
}