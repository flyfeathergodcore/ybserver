#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>

Config Config::Load(const std::string& path)
{
    Config cfg;

    try {
        YAML::Node root = YAML::LoadFile(path);
        auto srv = root["server"];

        if (srv["host"])       cfg.host     = srv["host"].as<std::string>();
        if (srv["port"])       cfg.port     = srv["port"].as<unsigned short>();
        if (srv["threads"])    cfg.threads  = srv["threads"].as<int>();
        if (srv["doc_root"])   cfg.doc_root = srv["doc_root"].as<std::string>();

        std::cout << "[config] 加载 " << path << std::endl;
    } catch (std::exception& e) {
        std::cerr << "[config] 加载失败，使用默认配置: " << e.what() << std::endl;
    }

    return cfg;
}
