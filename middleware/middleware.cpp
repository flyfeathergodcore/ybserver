#include "middleware/middleware.hpp"
#include "log/fast_logger.hpp"
#include "handler/metrics.hpp"
#include "net/session_region.hpp"
#include <cstring>
#include <ctime>
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
// RequestIdMiddleware — X-Request-Id forwarding / generation
// ═══════════════════════════════════════════════════════════════

std::string_view RequestIdMiddleware::GenerateId(SessionRegion& pool)
{
    thread_local static uint64_t counter = 0;
    counter++;
    // Compact ID: worker_tid_counter (hex) — written directly into pool
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lx_%lx",
                          (unsigned long)pthread_self(),
                          (unsigned long)counter);
    auto off = pool.DupOff({buf, static_cast<size_t>(n)});
    return pool.ToView(off);
}

Response RequestIdMiddleware::HandlePre(Context& ctx)
{
    auto existing = ctx.Header("x-request-id");
    if (!existing.empty()) {
        ctx.SetRequestId(existing);
        ctx.AddResponseHeader("X-Request-Id", existing);
    } else {
        auto* pool = ctx.Pool();
        if (pool) {
            auto id = GenerateId(*pool);
            ctx.SetRequestId(id);
            ctx.AddResponseHeader("X-Request-Id", id);
        }
    }
    return Response::None();
}

// ═══════════════════════════════════════════════════════════════
// LoggingMiddleware — structured JSON log
// ═══════════════════════════════════════════════════════════════

static std::string_view LogTimestamp(SessionRegion& pool)
{
    static time_t last = 0;
    static char buf[32];
    static size_t len = 0;
    auto now = ::time(nullptr);
    if (now != last) {
        last = now;
        struct tm tm;
        ::gmtime_r(&now, &tm);
        len = ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    auto off = pool.DupOff({buf, len});
    return pool.ToView(off);
}

asio::awaitable<void> LoggingMiddleware::HandlePost(
    const Context& ctx,
    int status_code,
    size_t bytes_sent,
    uint64_t elapsed_us,
    int /*worker_id*/)
{
    auto* pool = const_cast<Context&>(ctx).Pool();
    if (!pool)
        co_return;

    // Build JSON line — thread_local buffer, no per-request alloc
    thread_local std::string j;
    j.clear();
    j += R"({"t":")";
    j += LogTimestamp(*pool);          // pool-backed, no alloc
    j += R"(","m":")";
    j += ctx.Method();
    j += R"(","p":")";
    j += ctx.Path();
    j += R"(","s":)";

    // std::to_string → snprintf to stack buffer
    char num[24];
    int n;
    n = std::snprintf(num, sizeof(num), "%d", status_code);
    j.append(num, static_cast<size_t>(n));

    j += R"(,"d":)";
    n = std::snprintf(num, sizeof(num), "%lu", (unsigned long)elapsed_us);
    j.append(num, static_cast<size_t>(n));

    j += R"(,"b":)";
    n = std::snprintf(num, sizeof(num), "%zu", bytes_sent);
    j.append(num, static_cast<size_t>(n));

    j += R"(,"h2":)";
    j += ctx.IsHttp2() ? "true" : "false";
    j += R"(,"id":")";
    j += ctx.RequestId();
    j += '"';
    j += '}';
    FastLogger::Instance().Log(std::move(j));
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
