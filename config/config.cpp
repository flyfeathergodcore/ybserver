#include "config/config.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>

Config Config::Load(const std::string& path, bool strict)
{
    Config cfg;

    try {
        YAML::Node root = YAML::LoadFile(path);
        auto srv = root["server"];

        if (srv["host"])       cfg.host     = srv["host"].as<std::string>();
        if (srv["port"])       cfg.port     = srv["port"].as<unsigned short>();
        if (srv["tls_port"])   cfg.tls_port = srv["tls_port"].as<unsigned short>();
        if (srv["threads"])    cfg.threads  = srv["threads"].as<int>();
        if (srv["doc_root"])     cfg.doc_root     = srv["doc_root"].as<std::string>();
        if (srv["cpu_affinity"]) cfg.cpu_affinity = srv["cpu_affinity"].as<bool>();
        if (srv["max_body_size"]) cfg.max_body_size = srv["max_body_size"].as<size_t>();

        auto tls = srv["tls"];
        if (tls) {
            if (tls["cert"])  cfg.tls_cert = tls["cert"].as<std::string>();
            if (tls["key"])   cfg.tls_key  = tls["key"].as<std::string>();
        }

        // ── Proxy routes ──
        auto proxy = root["proxy"];
        if (proxy && proxy.IsSequence()) {
            for (size_t i = 0; i < proxy.size(); i++) {
                auto route = proxy[i];
                ProxyRoute pr;
                if (route["prefix"]) pr.prefix = route["prefix"].as<std::string>();

                auto parse_addr = [](const std::string& s) -> UpstreamAddr {
                    UpstreamAddr a;
                    auto colon = s.rfind(':');
                    if (colon != std::string::npos) {
                        a.host = s.substr(0, colon);
                        a.port = static_cast<unsigned short>(
                            std::stoi(s.substr(colon + 1)));
                    } else {
                        a.host = s;
                        a.port = 80;
                    }
                    return a;
                };

                // New format: upstreams (list)
                if (route["upstreams"] && route["upstreams"].IsSequence()) {
                    for (size_t j = 0; j < route["upstreams"].size(); j++) {
                        auto addr_s = route["upstreams"][j].as<std::string>();
                        pr.upstreams.push_back(parse_addr(addr_s));
                    }
                }
                // Old format: single upstream (string)
                else if (route["upstream"]) {
                    auto addr_s = route["upstream"].as<std::string>();
                    pr.upstreams.push_back(parse_addr(addr_s));
                }

                if (!pr.prefix.empty() && !pr.upstreams.empty()) {
                    cfg.proxy_routes.push_back(std::move(pr));
                }
            }
        }

        std::cout << "[config] 加载 " << path << std::endl;
        if (!cfg.proxy_routes.empty())
            std::cout << "[config] " << cfg.proxy_routes.size()
                      << " 代理路由已配置" << std::endl;
    } catch (std::exception& e) {
        if (strict) throw;  // dry-run mode: propagate error
        std::cerr << "[config] 加载失败，使用默认配置: " << e.what() << std::endl;
    }

    return cfg;
}
