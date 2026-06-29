#include <iostream>
#include <memory>
#include <sys/resource.h>
#include "config/config.hpp"
#include "cache/file_cache.hpp"
#include "handler/router.hpp"
#include "handler/request_handler.hpp"
#include "handler/reverse_proxy.hpp"
#include "handler/upstream_pool.hpp"
#include "middleware/middleware.hpp"
#include "handler/metrics.hpp"
#include "net/multi_server.hpp"
#include "ssl/tls_context.hpp"

int main(int argc, char* argv[])
{
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

        // ── Router ──
        Router router;

        // Static file handler (default route — lowest priority)
        {
            static FileCache s_cache;
            s_cache.LoadDirectory(cfg.doc_root);
            router.Add("/", std::make_unique<StaticFileHandler>(&s_cache));
        }

        // Proxy routes
        for (auto& pr : cfg.proxy_routes) {
            // Build upstream pool from configured servers
            std::vector<UpstreamServer> servers;
            for (auto& addr : pr.upstreams) {
                servers.push_back({addr.host, addr.port});
                std::cout << "[route] " << pr.prefix << " → "
                          << addr.host << ":" << addr.port << std::endl;
            }

            if (servers.empty()) continue;

            auto pool = std::make_unique<UpstreamPool>(std::move(servers));
            // Leak the pool — lives for server lifetime (simplified ownership)
            auto* leaked_pool = pool.release();  // NOLINT
            router.Add(pr.prefix,
                       std::make_unique<ReverseProxy>(*leaked_pool));
        }

        // ── Metrics collector ──
        auto collector = std::make_shared<MetricsCollector>(cfg.threads);

        // ── Middleware ──
        MiddlewareManager middleware;
        middleware.Add(std::make_unique<CORSMiddleware>());
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
