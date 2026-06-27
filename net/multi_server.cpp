#include "net/multi_server.hpp"
#include "net/session.hpp"
#include "http/llhttp_parser.hpp"
#include <iostream>
#include <memory>
#include <sys/socket.h>

MultiServer::MultiServer(const Config& cfg,
                         RequestHandler& handler,
                         MiddlewareChain& middleware,
                         std::shared_ptr<TlsContext> tls)
    : ServerBase(cfg, handler, middleware, std::move(tls)) {}

void MultiServer::EnableReusePort(tcp::acceptor& acceptor)
{
    int fd = acceptor.native_handle();
    if (fd != -1) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }
}

void MultiServer::Start()
{
    auto port = endpoint_.port();
    std::cout << "[server] HTTPS 监听 " << cfg_.host << ":" << port
              << " (" << cfg_.threads << " workers, SO_REUSEPORT)" << std::endl;

    workers_.reserve(static_cast<size_t>(cfg_.threads));

    for (int i = 0; i < cfg_.threads; ++i)
    {
        auto w = std::make_unique<Worker>();

        // Create acceptor on this worker's io_context
        w->acceptor = std::make_unique<tcp::acceptor>(w->ioctx);
        w->acceptor->open(endpoint_.protocol());
        EnableReusePort(*w->acceptor);
        w->acceptor->set_option(tcp::acceptor::reuse_address(true));
        w->acceptor->bind(endpoint_);
        w->acceptor->listen();

        w->pool = std::make_unique<SessionPool>();

        asio::co_spawn(w->ioctx, Listen(*w), asio::detached);

        w->thread = std::jthread([w = w.get()] {
            w->ioctx.run();
        });

        workers_.push_back(std::move(w));
    }

    // Block main thread until all workers finish
    for (auto& w : workers_) {
        w->thread.join();
    }
}

asio::awaitable<void> MultiServer::Listen(Worker& worker)
{
    auto exec = co_await asio::this_coro::executor;
    for (;;)
    {
        try
        {
            auto socket = co_await worker.acceptor->async_accept(
                asio::use_awaitable);

            asio::ssl::stream<tcp::socket> ss(std::move(socket),
                                              tls_->NativeContext());

            auto [ec] = co_await ss.async_handshake(
                asio::ssl::stream_base::server,
                asio::as_tuple(asio::use_awaitable));
            if (ec) continue;

            // 从池取 Session 或新建
            auto session = worker.pool->TryAcquireSession();
            if (session) {
                session->Reset(std::move(ss),
                               std::make_unique<LlhttpParser>());
            } else {
                session = std::make_shared<
                    Session<asio::ssl::stream<tcp::socket>>>(
                    std::move(ss), std::make_unique<LlhttpParser>(),
                    handler_, middleware_);
            }

            // Lambda 捕获 Session，结束后归还
            asio::co_spawn(exec,
                [session = std::move(session), pool = worker.pool.get()]()
                    -> asio::awaitable<void> {
                    co_await session->Start();
                    pool->ReleaseSession(std::move(session));
                },
                asio::detached);
        }
        catch (std::exception& e)
        {
            std::cerr << "[server] " << e.what() << std::endl;
        }
    }
}
