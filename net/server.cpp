#include "net/server.hpp"
#include "http/http1.1/session.hpp"
#include <iostream>
#include <memory>

Server::Server(asio::io_context& ioctx,
               const Config& cfg,
               Router& router,
               MiddlewareManager& middleware,
               std::shared_ptr<TlsContext> tls)
    : ServerBase(cfg, router, middleware, std::move(tls))
    , ioctx_(ioctx)
    , acceptor_(ioctx, endpoint_) {}

void Server::Start()
{
    auto port = endpoint_.port();
    std::cout << "[server] HTTPS 监听 " << cfg_.host << ":" << port << std::endl;
    asio::co_spawn(ioctx_, Listen(), asio::detached);
}

asio::awaitable<void> Server::Listen()
{
    auto exec = co_await asio::this_coro::executor;
    for (;;)
    {
        try
        {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);

            asio::ssl::stream<tcp::socket> ss(std::move(socket),
                                              tls_->NativeContext());

            auto [ec] = co_await ss.async_handshake(
                asio::ssl::stream_base::server,
                asio::as_tuple(asio::use_awaitable));
            if (ec) continue;

            auto session = std::make_shared<
                H11Session<asio::ssl::stream<tcp::socket>>>(
                std::move(ss),
                router_, middleware_);

            asio::co_spawn(exec, session->Start(), asio::detached);
        }
        catch (std::exception& e)
        {
            std::cerr << "[server] " << e.what() << std::endl;
        }
    }
}
