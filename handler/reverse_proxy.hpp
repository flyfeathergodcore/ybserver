#pragma once
#include "handler/request_handler.hpp"
#include "handler/upstream_pool.hpp"
#include <string>

// ═══════════════════════════════════════════════════════════════════
// ReverseProxy — HTTP/1.1 reverse proxy with load balancing
//
// Forwards incoming requests to an upstream picked from the pool,
// reads the response, and returns it to the caller.
//
// Handles:
//   - Round-robin load balancing (via UpstreamPool)
//   - Passive health checks (failure tracking with auto-suspend)
//   - Header forwarding (lowercased for H2 compatibility)
//   - Connection: close between proxy and upstream (for now)
// ═══════════════════════════════════════════════════════════════════

class ReverseProxy : public RequestHandler {
public:
    explicit ReverseProxy(UpstreamPool& pool);

    Response Handle(const Context& ctx) override;
    asio::awaitable<Response> HandleAsync(const Context& ctx) override;
    bool IsAsync() const override { return true; }

private:
    // Forward request to a single upstream, return HTTP response
    asio::awaitable<Response> Forward(const Context& ctx,
                                      asio::any_io_executor exec,
                                      std::string_view host,
                                      unsigned short port);

    UpstreamPool& pool_;
};
