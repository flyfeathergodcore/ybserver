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

struct RedirectRule {
    std::string from;                   // path prefix to match
    std::string to;                     // target URL (Location header)
    int code = 302;                     // 301|302|307|308
};

struct Config {
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    unsigned short tls_port = 0;         // 0 = TLS disabled
    int threads = 4;
    size_t max_body_size = 0;            // 0 = unlimited (bytes)
    std::string doc_root = "./www";
    std::string tls_cert;                // PEM certificate path
    std::string tls_key;                 // PEM private key path
    bool cpu_affinity = true;            // pin worker threads to dedicated cores

    // Proxy routes
    std::vector<ProxyRoute> proxy_routes;
    // Redirect rules (301/302/307/308)
    std::vector<RedirectRule> redirect_rules;

    /// Load config from YAML file.  When `strict` is true (used for -t / dry-run),
    /// throws exceptions on parse / file errors instead of silently defaulting.
    static Config Load(const std::string& path, bool strict = false);
};
