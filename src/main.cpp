//
// Created by davidcopperfield on 26-10-15.
//
#include <iostream>

#include "util/url.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: minicrawler <url>\n";
        return 1;
    }
    auto u = mc::parse_url(argv[1]);
    if (!u) {
        std::cerr << "invalid url\n";
        return 1;
    }
    std::cout << "scheme=" << u -> scheme << " host=" << u -> host
        << " port=" << u -> port << " path=" << u -> path
        << " query=" << u -> query << std::endl;
}