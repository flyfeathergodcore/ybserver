#pragma once
#include "net/server_base.hpp"
#include "net/session_pool.hpp"
#include "net/region_pool.hpp"
#include <thread>
#include <vector>

class MetricsCollector;

class MultiServer : public ServerBase {
public:
    MultiServer(const Config& cfg,
                RequestHandler& handler,
                MiddlewareChain& middleware,
                std::shared_ptr<TlsContext> tls,
                std::shared_ptr<MetricsCollector> metrics);

    void Start() override;

private:
    struct Worker {
        asio::io_context ioctx;
        std::unique_ptr<tcp::acceptor> acceptor;
        std::unique_ptr<SessionPool> pool;
        RegionPool region_pool;  // shared backing store for all Session regions
        std::jthread thread;
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    std::shared_ptr<MetricsCollector> metrics_;

    static void EnableReusePort(tcp::acceptor& acceptor);
    asio::awaitable<void> Listen(Worker& worker);
    asio::awaitable<void> FlushLoop(int worker_id);
};
