#include <iostream>
#include <memory>
#include <sys/resource.h>
#include <sys/stat.h>
#include "config/config.hpp"
#include "handler/router.hpp"
#include "handler/request_handler.hpp"
#include "middleware/middleware.hpp"
#include "handler/metrics.hpp"
#include "handler/ws_echo.hpp"
#include "net/multi_server.hpp"
#include "ssl/tls_context.hpp"

// ── Config validation (dry-run / -t) ──

static bool ValidateConfig(const Config& cfg, std::string& err)
{
    if (cfg.threads < 1) { err = "threads 必须 ≥ 1"; return false; }
    if (cfg.port == 0 && cfg.tls_port == 0) { err = "port 或 tls_port 必须设置"; return false; }

    struct stat st;
    if (!cfg.doc_root.empty() && ::stat(cfg.doc_root.c_str(), &st) != 0)
        { err = "doc_root 不存在: " + cfg.doc_root; return false; }

    // TLS checks
    if (cfg.tls_port > 0 || (!cfg.tls_cert.empty() && !cfg.tls_key.empty())) {
        if (cfg.tls_cert.empty()) { err = "tls.cert 未配置"; return false; }
        if (cfg.tls_key.empty())  { err = "tls.key 未配置"; return false; }
        if (::stat(cfg.tls_cert.c_str(), &st) != 0)
            { err = "tls.cert 文件不存在: " + cfg.tls_cert; return false; }
        if (::stat(cfg.tls_key.c_str(), &st) != 0)
            { err = "tls.key 文件不存在: " + cfg.tls_key; return false; }
    }

    // Redirect rule validation
    for (size_t i = 0; i < cfg.redirect_rules.size(); i++) {
        auto& r = cfg.redirect_rules[i];
        if (r.from.empty() || r.from[0] != '/') { err = "redirect[" + std::to_string(i) + "].from 必须以 / 开头"; return false; }
        if (r.to.empty()) { err = "redirect[" + std::to_string(i) + "].to 为空"; return false; }
        if (r.code != 301 && r.code != 302 && r.code != 307 && r.code != 308)
            { err = "redirect[" + std::to_string(i) + "].code 必须是 301/302/307/308"; return false; }
    }

    // Proxy route validation
    for (size_t i = 0; i < cfg.proxy_routes.size(); i++) {
        auto& r = cfg.proxy_routes[i];
        if (r.prefix.empty()) { err = "proxy[" + std::to_string(i) + "].prefix 为空"; return false; }
        if (r.prefix[0] != '/') { err = "proxy[" + std::to_string(i) + "].prefix 必须以 / 开头"; return false; }
        if (r.upstreams.empty()) { err = "proxy[" + std::to_string(i) + "] 没有 upstream"; return false; }
        for (size_t j = 0; j < r.upstreams.size(); j++) {
            auto& u = r.upstreams[j];
            if (u.host.empty()) { err = "proxy[" + std::to_string(i) + "].upstream[" + std::to_string(j) + "].host 为空"; return false; }
            if (u.port == 0) { err = "proxy[" + std::to_string(i) + "].upstream[" + std::to_string(j) + "] 端口无效"; return false; }
        }
    }

    return true;
}

int main(int argc, char* argv[])
{
    // ── Dry-run mode (-t) ──
    if (argc >= 2 && std::string_view(argv[1]) == "-t") {
        std::string cfg_path = argc > 2 ? argv[2] : "./config.yaml";
        std::cout << "[config] 正在验证 " << cfg_path << " ..." << std::endl;
        try {
            Config cfg = Config::Load(cfg_path, true);
            std::string err;
            if (ValidateConfig(cfg, err)) {
                std::cout << "[config] 配置验证通过 ✓" << std::endl;
                return 0;
            } else {
                std::cerr << "[config] 配置错误: " << err << std::endl;
                return 1;
            }
        } catch (std::exception& e) {
            std::cerr << "[config] 加载失败: " << e.what() << std::endl;
            return 1;
        }
    }

    try
    {
        // Raise file descriptor limit (soft) to the hard limit.
        {
            struct rlimit rl;
            if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
                rl.rlim_cur = rl.rlim_max;
                ::setrlimit(RLIMIT_NOFILE, &rl);
            }
        }

        std::string cfg_path = argc > 1 ? argv[1] : "./config.yaml";
        Config cfg = Config::Load(cfg_path);

        // ── Router: auto-register static files + proxy routes from config ──
        Router router;
        router.SetupFromConfig(cfg);
        router.Add("/echo", std::unique_ptr<WsEchoHandler>(new WsEchoHandler()));
        std::cout << "[route] /echo → WsEchoHandler" << std::endl;

        // ── Metrics collector ──
        auto collector = std::make_shared<MetricsCollector>(cfg.threads);

        // ── Middleware ──
        MiddlewareManager middleware;
        middleware.Add(std::make_unique<CORSMiddleware>());
        middleware.Add(std::make_unique<RequestIdMiddleware>());
        middleware.Add(std::make_unique<MetricsMiddleware>(collector.get()));
        middleware.Add(std::make_unique<LoggingMiddleware>());

        // ── TLS ──
        if (cfg.tls_cert.empty() || cfg.tls_key.empty()) {
            std::cerr << "必须配置 tls.cert 和 tls.key" << std::endl;
            return 1;
        }
        auto tls = std::make_shared<TlsContext>();
        if (!tls->Load(cfg.tls_cert, cfg.tls_key)) {
            return 1;
        }

        MultiServer server(cfg, router, middleware, tls, collector);
        server.Start();
    }
    catch (std::exception& e)
    {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
