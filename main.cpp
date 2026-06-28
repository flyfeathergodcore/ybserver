#include <iostream>
#include <memory>
#include "config/config.hpp"
#include "cache/file_cache.hpp"
#include "handler/request_handler.hpp"
#include "middleware/middleware.hpp"
#include "net/metrics.hpp"
#include "net/multi_server.hpp"
#include "net/tls_context.hpp"

int main(int argc, char* argv[])
{
    try
    {
        std::string cfg_path = argc > 1 ? argv[1] : "./config.yaml";
        Config cfg = Config::Load(cfg_path);

        FileCache cache;
        cache.LoadDirectory(cfg.doc_root);

        StaticFileHandler handler(&cache);

        // ── Metrics collector (shared across middleware + server) ──
        auto collector = std::make_shared<MetricsCollector>(cfg.threads);

        // ── 中间件链 ──
        MiddlewareChain middleware;
        middleware.Add(std::make_unique<MetricsMiddleware>(collector.get()));
        middleware.Add(std::make_unique<LoggingMiddleware>());
        middleware.Add(std::make_unique<CORSMiddleware>());

        // ── TLS（必需） ──
        if (cfg.tls_cert.empty() || cfg.tls_key.empty()) {
            std::cerr << "必须配置 tls.cert 和 tls.key" << std::endl;
            return 1;
        }
        auto tls = std::make_shared<TlsContext>();
        if (!tls->Load(cfg.tls_cert, cfg.tls_key)) {
            return 1;
        }

        MultiServer server(cfg, handler, middleware, tls, collector);
        server.Start();
    }
    catch (std::exception& e)
    {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
