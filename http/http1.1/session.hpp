#pragma once
#include "net/session_base.hpp"
#include "http/http1.1/parser.hpp"
#include <asio/ssl.hpp>
#include <array>

class RegionPool;

using asio::ip::tcp;

template<typename Stream>
class H11Session : public SessionBase {
public:
    H11Session(Stream stream,
               Router& router,
               MiddlewareManager& middleware,
               RegionPool* region_pool = nullptr);

    asio::awaitable<void> Start() override;

    /// Reuse session shell: replace stream, reuse existing parser.
    void Reset(Stream stream) {
        stream_ = std::move(stream);
    }

private:
    Stream stream_;
    H1Parser parser_;

    // Error write — known code (400/413/426/500), once per connection, coroutine frame cost OK
    asio::awaitable<void> WriteError(int code);
    // Middleware / custom response write (raw middleware, 101 upgrade)
    asio::awaitable<void> WriteError(Response resp);
    // Full send — file (sendfile), stream (SSE), or regular header+body
    asio::awaitable<void> Send(Response response);
};
