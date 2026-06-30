#pragma once
#include "handler/request_handler.hpp"
#include "handler/upstream_pool.hpp"
#include "config/config.hpp"
#include <memory>
#include <string>
#include <vector>

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
    /// Create from config upstream addresses (common case).
    explicit ReverseProxy(std::vector<UpstreamAddr> upstreams);

    /// Create from pre-built pool (advanced use).
    explicit ReverseProxy(UpstreamPool& pool);

    Response Handle(const Context& ctx) override;
    asio::awaitable<Response> HandleAsync(const Context& ctx) override;
    bool IsAsync() const override { return true; }

private:
    asio::awaitable<Response> Forward(const Context& ctx,
                                      asio::any_io_executor exec,
                                      std::string_view host,
                                      unsigned short port);

    std::unique_ptr<UpstreamPool> owned_pool_;  // owns the pool (when constructed from addresses)
    UpstreamPool* pool_;                         // non-owning reference
};
