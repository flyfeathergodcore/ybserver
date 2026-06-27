#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <memory>
#include "config/config.hpp"
#include "handler/request_handler.hpp"
#include "middleware/middleware.hpp"
#include "net/tls_context.hpp"

using asio::ip::tcp;

class ServerBase {
public:
    ServerBase(const Config& cfg,
               RequestHandler& handler,
               MiddlewareChain& middleware,
               std::shared_ptr<TlsContext> tls)
        : cfg_(cfg)
        , handler_(handler)
        , middleware_(middleware)
        , tls_(std::move(tls))
    {
        auto port = cfg_.tls_port > 0 ? cfg_.tls_port : cfg_.port;
        endpoint_ = tcp::endpoint(asio::ip::make_address(cfg_.host), port);
    }

    virtual ~ServerBase() = default;
    virtual void Start() = 0;

protected:
    const Config& cfg_;
    RequestHandler& handler_;
    MiddlewareChain& middleware_;
    std::shared_ptr<TlsContext> tls_;
    tcp::endpoint endpoint_;
};
