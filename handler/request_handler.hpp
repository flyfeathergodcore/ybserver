#pragma once
#include <string>
#include <asio.hpp>
#include "net/response.hpp"
#include "http/context.hpp"
#include "cache/file_cache.hpp"
#include "config/config.hpp"
#include "net/ws_connection.hpp"

// ═══════════════════════════════════════════════════════════════════
// RequestHandler — abstract interface for HTTP request handlers
//
// Two execution paths:
//   Handle()       — sync, for CPU-bound handlers (static files, etc.)
//   HandleAsync()  — async coroutine, for I/O-bound handlers (proxy, etc.)
//
// Override IsAsync() to return true when using HandleAsync, so the
// session dispatcher picks the coroutine path.
// ═══════════════════════════════════════════════════════════════════

class RequestHandler {
public:
    virtual ~RequestHandler() = default;

    /// Fast sync path — override for CPU-bound handlers.
    /// Pure virtual: every handler must implement at least this.
    virtual Response Handle(const Context& ctx) = 0;

    /// Async coroutine path — override for I/O-bound handlers (proxy, etc.).
    /// Default implementation wraps Handle() — callable from any handler
    /// regardless of IsAsync(), but the coroutine frame overhead only pays
    /// off when overridden.
    virtual asio::awaitable<Response> HandleAsync(const Context& ctx) {
        co_return Handle(ctx);
    }

    /// Returns true if this handler should use the async path.
    /// Session checks this to decide whether to co_await HandleAsync().
    virtual bool IsAsync() const { return false; }

    /// WebSocket handler — session calls this after 101 handshake.
    /// Default empty: handler does not support WebSocket.
    /// Override to read/write frames via the connection object.
    /// @param ctx  The original HTTP upgrade request context
    /// @param conn WebSocket connection for frame read/write
    virtual asio::awaitable<void> HandleWebSocket(const Context& /*ctx*/,
                                                    WsConnectionBase& /*conn*/) {
        co_return;
    }
};

// ═══════════════════════════════════════════════════════════════════
// StaticFileHandler — serves static files from a FileCache
// ═══════════════════════════════════════════════════════════════════

class StaticFileHandler : public RequestHandler {
public:
    explicit StaticFileHandler(const FileCache* cache);
    Response Handle(const Context& ctx) override;

private:
    const FileCache* cache_;
    std::string NormalizePath(std::string_view raw) const;
};

// ═══════════════════════════════════════════════════════════════════
// NullHandler — returns 404 for every request (router fallback)
// ═══════════════════════════════════════════════════════════════════

class NullHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override {
        return Response::Error(404, *ctx.Pool());
    }
};

// ═══════════════════════════════════════════════════════════════════
// RedirectHandler — returns 301/302/307/308 + Location header
// ═══════════════════════════════════════════════════════════════════

class RedirectHandler : public RequestHandler {
public:
    RedirectHandler(std::string target, int code = 302)
        : target_(std::move(target)), code_(code) {}
    Response Handle(const Context& ctx) override;
private:
    std::string target_;
    int code_;
};
