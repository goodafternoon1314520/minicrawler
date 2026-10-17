#pragma once

#include <string>
#include <vector>

namespace mc {
    // 提取 <a href="..."> 的原始href值（不做URL解析，保持页面里写的样子）
    std::vector<std::string> extract_links(const std::string& html);
}