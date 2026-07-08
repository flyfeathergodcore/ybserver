#pragma once
#include <string>
#include <asio.hpp>
#include "net/response.hpp"
#include "http/context.hpp"
#include "cache/file_cache.hpp"
#include "config/config.hpp"
#include "net/ws_connection.hpp"

// ── 前向声明 ──
class StreamSink;

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

    // ── 流式路径 ──
    /// Returns true if this handler uses streaming output.
    virtual bool IsStream() const { return false; }

    /// Streaming entry point.
    /// Handler writes chunks via sink; session does not call Send() afterwards.
    /// Default fallback: collect full response from HandleAsync() then flush.
    virtual asio::awaitable<void> HandleStream(const Context& ctx,
                                                StreamSink& sink);

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
    explicit RedirectHandler(std::string target, int code = 302)
        : target_(std::move(target)), code_(code) {}
    Response Handle(const Context& ctx) override;
private:
    std::string target_;
    int code_;
};

// ═══════════════════════════════════════════════════════════════════
// StreamSink — 流式输出通道
//
// Handler 通过此接口分块写回客户端。
// H1/H2 Session 各自提供具体实现，擦除底层流类型差异。
// ═══════════════════════════════════════════════════════════════════

class StreamSink {
public:
    virtual ~StreamSink() = default;

    /// 写原始 bytes 到响应流
    /// @return true 成功，false 连接断开或出错
    virtual asio::awaitable<bool> Write(std::string_view data) = 0;

    /// 写 SSE 格式数据：自动包装为 "data: <content>\n\n"
    virtual asio::awaitable<bool> PushSSE(std::string_view data) {
        std::string frame = "data: ";
        frame.append(data.data(), data.size());
        frame.append("\n\n");
        co_return co_await Write(frame);
    }

    /// 关闭流
    virtual void End() = 0;

    /// 客户端是否已断开
    virtual bool IsDisconnected() const = 0;
};

// ── RequestHandler::HandleStream 默认实现（需要 StreamSink 完整定义）──

inline asio::awaitable<void> RequestHandler::HandleStream(
    const Context& ctx, StreamSink& sink) {
    auto resp = co_await HandleAsync(ctx);
    co_await sink.Write(resp.BodyWire());
    sink.End();
}
