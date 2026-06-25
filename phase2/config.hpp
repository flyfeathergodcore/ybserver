#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>

struct Config {
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    int threads = 4;
    std::string doc_root = "./www";

    static Config Load(const std::string& path);
};

#endif
