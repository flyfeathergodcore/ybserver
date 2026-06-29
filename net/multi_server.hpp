#pragma once
#include "net/server_base.hpp"
#include "http/http1.1/session_pool.hpp"
#include "net/region_pool.hpp"
#include "handler/metrics.hpp"
#include <thread>
#include <vector>

class MultiServer : public ServerBase {
public:
    MultiServer(const Config& cfg,
                Router& router,
                MiddlewareManager& middleware,
                std::shared_ptr<TlsContext> tls,
                std::shared_ptr<MetricsCollector> metrics);

    void Start() override;

private:
    struct Worker {
        asio::io_context ioctx;
        std::unique_ptr<tcp::acceptor> acceptor;
        std::unique_ptr<H11SessionPool> pool;
        RegionPool region_pool;
        std::jthread thread;
    };

    static void EnableReusePort(tcp::acceptor& acceptor);
    asio::awaitable<void> Listen(Worker& worker);
    asio::awaitable<void> FlushLoop(int worker_id);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::shared_ptr<MetricsCollector> metrics_;
};
