#pragma once
#include <string>
#include <asio.hpp>
#include "net/response.hpp"
#include "http/context.hpp"
#include "cache/file_cache.hpp"
#include "config/config.hpp"

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
