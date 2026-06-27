#pragma once
#include "net/server_base.hpp"
#include "net/session_pool.hpp"
#include <thread>
#include <vector>

class MultiServer : public ServerBase {
public:
    MultiServer(const Config& cfg,
                RequestHandler& handler,
                MiddlewareChain& middleware,
                std::shared_ptr<TlsContext> tls);

    void Start() override;

private:
    struct Worker {
        asio::io_context ioctx;
        std::unique_ptr<tcp::acceptor> acceptor;
        std::unique_ptr<SessionPool> pool;
        std::jthread thread;
    };

    std::vector<std::unique_ptr<Worker>> workers_;

    static void EnableReusePort(tcp::acceptor& acceptor);
    asio::awaitable<void> Listen(Worker& worker);
};
