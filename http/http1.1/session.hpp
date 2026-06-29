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
    asio::awaitable<void> Send(Response response);

    Stream stream_;
    H1Parser parser_;
};
