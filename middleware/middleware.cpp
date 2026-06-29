#include "middleware/middleware.hpp"
#include "log/fast_logger.hpp"
#include "handler/metrics.hpp"
#include "net/session_region.hpp"
#include <cstring>
#include <iostream>

// ═══════════════════════════════════════════════════════════════
// MiddlewareManager
// ═══════════════════════════════════════════════════════════════

void MiddlewareManager::Add(std::unique_ptr<Middleware> mw)
{
    auto type = mw->GetType();
    auto* ptr = mw.get();

    if (type == Middleware::Type::PreRequest ||
        type == Middleware::Type::Both)
        pre_.push_back(ptr);

    if (type == Middleware::Type::PostResponse ||
        type == Middleware::Type::Both)
        post_.push_back(ptr);

    owned_.push_back(std::move(mw));
}

Response MiddlewareManager::ProcessRaw(const char* data, size_t len)
{
    for (auto* mw : pre_) {
        // PreRequest middlewares may also implement OnRawData.
        // We iterate all owned middlewares for raw data.
    }
    for (auto& mw : owned_) {
        if (auto resp = mw->OnRawData(data, len); !resp.IsNone())
            return resp;
    }
    return Response::None();
}

Response MiddlewareManager::ExecutePre(Context& ctx)
{
    for (auto* mw : pre_) {
        auto resp = mw->HandlePre(ctx);
        if (!resp.IsNone())
            return resp;
    }
    return Response::None();
}

asio::awaitable<void> MiddlewareManager::ExecutePost(
    const Context& ctx, int status_code,
    size_t bytes_sent, uint64_t elapsed_us, int worker_id)
{
    for (auto* mw : post_) {
        co_await mw->HandlePost(ctx, status_code, bytes_sent, elapsed_us, worker_id);
    }
}

// ═══════════════════════════════════════════════════════════════
// CORSMiddleware
// ═══════════════════════════════════════════════════════════════

Response CORSMiddleware::HandlePre(Context& ctx)
{
    if (ctx.Method() == "OPTIONS") {
        return Response::Raw(204,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    }

    // Inject CORS header — handler will read it via ctx.ResponseHeaders()
    // and write it to the response via resp.Header().
    ctx.AddResponseHeader("Access-Control-Allow-Origin", "*");
    return Response::None();
}

// ═══════════════════════════════════════════════════════════════
// LoggingMiddleware
// ═══════════════════════════════════════════════════════════════

asio::awaitable<void> LoggingMiddleware::HandlePost(
    const Context& ctx, int /*status_code*/,
    size_t /*bytes_sent*/, uint64_t /*elapsed_us*/,
    int /*worker_id*/)
{
    FastLogger::Instance().Log(
        std::string(ctx.Method()) + " " + std::string(ctx.Path()));
    co_return;
}

// ═══════════════════════════════════════════════════════════════
// MetricsMiddleware
// ═══════════════════════════════════════════════════════════════

Response MetricsMiddleware::HandlePre(Context& ctx)
{
    auto path = ctx.Path();

    if (path == "/metrics.json")
    {
        auto json = collector_->RenderMetricsJson();
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(200, std::move(json));

        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", json.size());
        resp.EndHeaders();
        pool->Write(json);
        return resp;
    }

    // Redirect /dashboard → /dashboard/
    if (path == "/dashboard")
    {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(301,
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: /dashboard/\r\n"
            "Content-Length: 0\r\n\r\n");
        Response resp(301, *pool);
        resp.Header("Location", "/dashboard/");
        resp.EndHeaders();
        return resp;
    }

    if (path == "/metrics/stream" || path == "/metrics/stream/")
    {
        auto* pool = ctx.Pool();
        if (!pool)
            return Response::Raw(200, "data: {\"error\":\"no pool\"}\n\n");

        int interval_ms = kDefaultPushMs;
        return Response::SSEStream(*pool, interval_ms);
    }

    return Response::None();
}

asio::awaitable<void> MetricsMiddleware::HandlePost(
    const Context& ctx,
    int status_code,
    size_t bytes_sent,
    uint64_t elapsed_us,
    int worker_id)
{
    if (collector_)
        collector_->OnRequest(elapsed_us, status_code, bytes_sent, worker_id,
                               ctx.IsHttp2());
    co_return;
}
