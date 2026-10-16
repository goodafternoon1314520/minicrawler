//
// Created by davidcopperfield on 26-10-15.
//
#include <iostream>

#include "net/http_client.h"
#include "util/url.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: minicrawler <url>\n";
        return 1;
    }
    auto url = mc::parse_url(argv[1]);
    if (!url) {
        std::cerr << "invalid url\n";
        return 1;
    }

    std::string err;
    auto resp = mc::http_get(*url, &err);
    if (!resp) {
        std::cerr << "request failed: " << err << std::endl;
        return 2;
    }

    std::cout << "HTTP " << resp->status << " " << resp->reason << std::endl;
    for (const auto& [k, v] : resp->headers)
        std::cout << k << ": " << v << "\n";
    std::cout << "\nbody (" << resp->body.size() << "bytes):\n" << resp->body.substr(0, 300) << "\n";
}