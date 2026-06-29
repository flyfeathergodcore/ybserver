#pragma once
#include <string>
#include <vector>

struct UpstreamAddr {
    std::string host;
    unsigned short port = 0;
};

struct ProxyRoute {
    std::string prefix;                 // path prefix, e.g. "/api/"
    std::vector<UpstreamAddr> upstreams;
};

struct Config {
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    unsigned short tls_port = 0;         // 0 = TLS disabled
    int threads = 4;
    std::string doc_root = "./www";
    std::string tls_cert;                // PEM certificate path
    std::string tls_key;                 // PEM private key path
    bool cpu_affinity = true;            // pin worker threads to dedicated cores

    // Proxy routes
    std::vector<ProxyRoute> proxy_routes;

    static Config Load(const std::string& path);
};
