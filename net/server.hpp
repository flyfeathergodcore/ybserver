#pragma once
#include "net/server_base.hpp"

class Server : public ServerBase {
public:
    Server(asio::io_context& ioctx,
           const Config& cfg,
           RequestHandler& handler,
           MiddlewareChain& middleware,
           std::shared_ptr<TlsContext> tls);

    void Start() override;

private:
    asio::awaitable<void> Listen();

    asio::io_context& ioctx_;
    tcp::acceptor acceptor_;
};
