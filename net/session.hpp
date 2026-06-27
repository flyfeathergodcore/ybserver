#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <memory>
#include <array>
#include "http/context.hpp"
#include "net/response.hpp"
#include "net/mem_pool.hpp"
#include "handler/request_handler.hpp"
#include "middleware/middleware.hpp"

using asio::ip::tcp;

template<typename Stream>
class Session : public std::enable_shared_from_this<Session<Stream>> {
public:
    Session(Stream stream,
            std::unique_ptr<Context> parser,
            RequestHandler& handler,
            MiddlewareChain& middleware);

    asio::awaitable<void> Start();

    // 复用 Session 对象：替换 stream 和 parser，保持 handler/middleware
    void Reset(Stream stream, std::unique_ptr<Context> parser) {
        stream_ = std::move(stream);
        parser_ = std::move(parser);
    }

    /// Per-connection memory pool, reset after each keep-alive request.
    MemPool& Pool() { return pool_; }

private:
    asio::awaitable<void> Send(Response response);

    Stream stream_;
    std::unique_ptr<Context> parser_;
    RequestHandler& handler_;
    MiddlewareChain& middleware_;
    MemPool pool_;
};
