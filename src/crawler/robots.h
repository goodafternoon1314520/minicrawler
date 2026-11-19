// robots.txt 解析与匹配（纯函数）

#pragma once
#include <optional>
#include <string>
#include <vector>

namespace mc {

    class RobotsPolicy {
    public:
        struct Rule {
            bool allow;
            std::string pattern;
        };

        static RobotsPolicy allow_all() { return RobotsPolicy(); }
        static RobotsPolicy disallow_all();
        // agent 是我们的 product token，例如 "minicrawler"（不区分大小写）
        static RobotsPolicy parse(const std::string& text, const std::string& agent);

        // 参数是 "路径[?查询]"，例如 "/a/b?x=1"
        bool allowed(const std::string& path_and_query) const;
        std::optional<double> crawl_delay() const { return crawl_delay_; }

    private:
        std::vector<Rule> rules_;
        std::optional<double> crawl_delay_;
    };

}  // namespace mc