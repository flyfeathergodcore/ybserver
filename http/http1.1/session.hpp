#pragma once
#include "handler/request_handler.hpp"
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

// ═══════════════════════════════════════════════════════════════════
// H1StreamSink — H1 的 StreamSink 实现
// 类型擦除：Stream 可以是 tcp::socket 或 ssl::stream<tcp::socket>
// ═══════════════════════════════════════════════════════════════════

template<typename Stream>
class H1StreamSink : public StreamSink {
public:
    explicit H1StreamSink(Stream& stream)
        : stream_(stream) {}

    asio::awaitable<bool> Write(std::string_view data) override {
        if (disconnected_) co_return false;
        auto [ec, n] = co_await asio::async_write(
            stream_, asio::buffer(data),
            asio::as_tuple(asio::use_awaitable));
        if (ec) disconnected_ = true;
        co_return !ec;
    }

    void End() override { disconnected_ = true; }

    bool IsDisconnected() const override { return disconnected_; }

private:
    Stream& stream_;
    bool disconnected_ = false;
};
