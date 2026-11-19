#include <gtest/gtest.h>

#include <string>

#include "crawler/robots.h"

using mc::RobotsPolicy;

static RobotsPolicy P(const std::string& text, const std::string& agent = "minicrawler") {
    return RobotsPolicy::parse(text, agent);
}

TEST(RobotsTest, EmptyMeansAllowAll) {
    EXPECT_TRUE(P("").allowed("/anything"));
    EXPECT_TRUE(RobotsPolicy::allow_all().allowed("/"));
}

TEST(RobotsTest, DisallowAll) {
    EXPECT_FALSE(RobotsPolicy::disallow_all().allowed("/"));
    EXPECT_FALSE(RobotsPolicy::disallow_all().allowed("/a/b"));
}

TEST(RobotsTest, PrefixMatching) {
    auto p = P("User-agent: *\nDisallow: /private\n");
    EXPECT_FALSE(p.allowed("/private"));
    EXPECT_FALSE(p.allowed("/private/x"));
    EXPECT_FALSE(p.allowed("/private-things"));  // 前缀匹配，不是"目录"匹配
    EXPECT_TRUE(p.allowed("/public"));
    EXPECT_TRUE(p.allowed("/"));
}

TEST(RobotsTest, EmptyDisallowAllowsEverything) {
    EXPECT_TRUE(P("User-agent: *\nDisallow:\n").allowed("/anything"));
}

TEST(RobotsTest, LongestMatchWinsAndTieGoesToAllow) {
    auto p = P("User-agent: *\nDisallow: /a\nAllow: /a/b\n");
    EXPECT_FALSE(p.allowed("/a/c"));
    EXPECT_TRUE(p.allowed("/a/b/x"));

    EXPECT_TRUE(P("User-agent: *\nAllow: /a/b\nDisallow: /a\n").allowed("/a/b/x"));  // 与书写顺序无关
    EXPECT_TRUE(P("User-agent: *\nDisallow: /same\nAllow: /same\n").allowed("/same/x"));  // 一样长：Allow 胜
}

TEST(RobotsTest, WildcardAndEndAnchor) {
    auto p = P("User-agent: *\nDisallow: /*.pdf$\nDisallow: /*?\nDisallow: /tmp/*/cache\n");
    EXPECT_FALSE(p.allowed("/docs/a.pdf"));
    EXPECT_TRUE(p.allowed("/docs/a.pdf.html"));  // '$' 要求恰好以 .pdf 结尾
    EXPECT_FALSE(p.allowed("/s?q=1"));
    EXPECT_TRUE(p.allowed("/s"));
    EXPECT_FALSE(p.allowed("/tmp/x/y/cache/z"));
    EXPECT_TRUE(p.allowed("/tmp/cache"));
}

TEST(RobotsTest, EndAnchorOnExactPath) {
    auto p = P("User-agent: *\nDisallow: /p/1$\n");
    EXPECT_FALSE(p.allowed("/p/1"));
    EXPECT_TRUE(p.allowed("/p/10"));
    EXPECT_TRUE(p.allowed("/p/1/x"));
}

TEST(RobotsTest, SpecificGroupBeatsWildcardGroup) {
    const char* txt = "User-agent: *\nDisallow: /\n\nUser-agent: MiniCrawler\nDisallow: /secret\n";
    EXPECT_TRUE(P(txt).allowed("/open"));
    EXPECT_FALSE(P(txt).allowed("/secret/x"));
    EXPECT_FALSE(P(txt, "otherbot").allowed("/open"));  // 没有专属分组，回退到 "*"
}

TEST(RobotsTest, ConsecutiveUserAgentLinesShareRules) {
    const char* txt = "User-agent: foo\nUser-agent: minicrawler\n\nDisallow: /x\n";  // 空行不结束分组
    EXPECT_FALSE(P(txt).allowed("/x"));
    EXPECT_TRUE(P(txt, "bar").allowed("/x"));  // bar 没有分组也没有 "*"：放行
}

TEST(RobotsTest, GroupsOfTheSameAgentAreMerged) {
    auto p = P("User-agent: minicrawler\nDisallow: /a\n\nUser-agent: minicrawler\nDisallow: /b\n");
    EXPECT_FALSE(p.allowed("/a"));
    EXPECT_FALSE(p.allowed("/b"));
    EXPECT_TRUE(p.allowed("/c"));
}

TEST(RobotsTest, SyntaxLeniency) {
    const std::string t =
            "\xEF\xBB\xBF"
            "DISALLOW: /before-any-group\r\n"  // 出现在任何 User-agent 之前：忽略
            "# a comment\r\n"
            "USER-AGENT: *   # trailing comment\r\n"
            "disallow : /a # c\r\n"
            "this line has no colon\r\n"
            "Sitemap: http://x/s.xml\r\n";
    auto p = P(t);
    EXPECT_FALSE(p.allowed("/a"));
    EXPECT_TRUE(p.allowed("/before-any-group"));
}

TEST(RobotsTest, CrawlDelay) {
    auto d = P("User-agent: *\nCrawl-delay: 2.5\n").crawl_delay();
    ASSERT_TRUE(d.has_value());
    EXPECT_DOUBLE_EQ(*d, 2.5);
    EXPECT_FALSE(P("User-agent: *\nCrawl-delay: abc\n").crawl_delay().has_value());
    EXPECT_FALSE(P("User-agent: *\nCrawl-delay: -1\n").crawl_delay().has_value());
    EXPECT_FALSE(P("").crawl_delay().has_value());
}

TEST(RobotsTest, PathologicalPatternIsFast) {
    // 朴素回溯在这里是指数级的；动态规划是 O(|pattern| * |path|)
    auto p = P("User-agent: *\nDisallow: /*a*a*a*a*a*a*a*a*a*a*b\n");
    EXPECT_TRUE(p.allowed("/" + std::string(5000, 'a')));
}