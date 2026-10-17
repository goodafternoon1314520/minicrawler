//
// Created by davidcopperfield on 26-10-15.
//
#include <iostream>
#include <cstdlib>
#include <string>

#include "net/http_client.h"
#include "util/url.h"
#include "crawler/crawler.h"

namespace {
    void usage() {
        std::cerr << "Usage: minicrawler <seed_url> [--depth N] [--max-pages N]\n"
                     "                              [--delay-ms N] [--any-host]\n";
    }

    bool to_long(const char *s, long *out) {
        char *end = nullptr;
        long v = std::strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0')
            return false;
        *out = v;
        return true;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    auto seed = mc::parse_url(argv[1]);
    if (!seed) {
        std::cerr << "invalid url\n";
        return 1;
    }

    mc::CrawlOptions opt;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        long v = 0;
        if (a == "--any-host")
            opt.same_host_only = false;
        else if ((a == "--depth" || a == "--max-pages" || a == "--delay-ms") && i + 1 < argc && to_long(argv[i + 1], &v) && v >= 0) {
            ++i;
            if (a == "--depth")
                opt.max_depth = static_cast<int>(v);
            else if (a == "--max-pages")
                opt.max_pages = static_cast<size_t>(v);
            else
                opt.delay_ms = static_cast<int>(v);
        }
        else {
            usage();
            return 1;
        }
    }

    mc::Crawler crawler(opt, mc::default_fetcher());
    auto stats = crawler.run(*seed, [](const mc::PageResult& r){
        if (r.status == 0)
            std::cout << "[d=" << r.depth << "] ERR " << r.url << " (" << r.error << ")\n";
        else
            std::cout << "[d=" << r.depth << "] " << r.status << "  " << r.url << " (" << r.bytes << " bytes, " << r.links_found << " links)\n";
    });

    std::cout << "\ndone: fetched=" << stats.fetched << " failed=" << stats.failed << " skipped_non_http=" << stats.skipped_unsupported << std::endl;
}