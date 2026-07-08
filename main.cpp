#include <iostream>
#include <memory>
#include <sys/resource.h>
#include "config/config.hpp"
#include "handler/router.hpp"
#include "handler/request_handler.hpp"
#include "middleware/middleware.hpp"
#include "handler/metrics.hpp"
#include "handler/ws_echo.hpp"
#include "net/multi_server.hpp"
#include "ssl/tls_context.hpp"
#ifdef __has_include
#if __has_include("rpc/grpc_channel_pool.hpp")
#include "rpc/grpc_channel_pool.hpp"
#include "examples/cpp_handlers/chat_handler.hpp"
#define HAS_GRPC 1
#endif
#endif

    // ── Dry-run mode (-t) ──
int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "-t") {
        return Config::DryRun(argc > 2 ? argv[2] : "./config.yaml");
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

        // ── 热插拔 .so + 热重载 ──
        router.LoadPlugins({"./build/handlers", "./handlers"});
        router.WatchPlugins({"./build/handlers", "./handlers"});

#if HAS_GRPC
        // ── gRPC AI 服务（ChatHandler → ai-chat） ──
        try {
            auto exec = asio::system_executor();
            auto bridge = std::make_shared<GrpcBridge>(exec);
            auto pool = std::make_shared<GrpcChannelPool>(std::move(bridge));
            auto chat_handler = std::make_unique<ChatHandler>(std::move(pool));
            router.Add("/v1/chat", std::move(chat_handler));
            std::cout << "[route] /v1/chat → ChatHandler (gRPC → ai-chat:50051)" << std::endl;
        } catch (std::exception& e) {
            std::cerr << "[warn] gRPC ChatHandler init failed: " << e.what() << std::endl;
        }
#endif

        // ── Metrics collector ──
        auto collector = std::make_shared<MetricsCollector>(cfg.threads);

        // ── Middleware（自动添加 4 个默认组件） ──
        MiddlewareManager middleware;
        middleware.InitDefaults(collector.get());

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

        router.StopWatch();
    }
    catch (std::exception& e)
    {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
