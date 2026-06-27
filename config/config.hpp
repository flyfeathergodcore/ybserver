#pragma once
#include <string>

struct Config {
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    unsigned short tls_port = 0;         // 0 = TLS disabled
    int threads = 4;
    std::string doc_root = "./www";
    std::string tls_cert;                // PEM certificate path
    std::string tls_key;                 // PEM private key path

    static Config Load(const std::string& path);
};
