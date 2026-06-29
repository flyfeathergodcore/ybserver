#pragma once
#include "handler/request_handler.hpp"
#include <string>
#include <asio.hpp>

// ═══════════════════════════════════════════════════════════════════
// ProxyHandler — reverse proxy to an upstream HTTP server
//
// Forwards incoming HTTP/1.1 requests to the configured upstream,
// reads the response, and returns it to the caller.
//
// Uses HandleAsync() for non-blocking upstream I/O.  Resolver and
// socket are created on-the-fly from the calling coroutine's executor,
// so no external io_context is needed.
// ═══════════════════════════════════════════════════════════════════

struct UpstreamConfig {
    std::string host;       // e.g. "127.0.0.1"
    unsigned short port;    // e.g. 3000
};

class ProxyHandler : public RequestHandler {
public:
    explicit ProxyHandler(UpstreamConfig upstream);

    Response Handle(const Context& ctx) override;
    asio::awaitable<Response> HandleAsync(const Context& ctx) override;
    bool IsAsync() const override { return true; }

private:
    UpstreamConfig upstream_;
};
