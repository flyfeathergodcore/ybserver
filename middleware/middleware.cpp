#include "middleware/middleware.hpp"
#include "log/fast_logger.hpp"
#include "net/metrics.hpp"
#include "net/session_region.hpp"
#include <cstring>
#include <iostream>

// ── Middleware 基类默认实现 ──

Response Middleware::Handle(const Context& ctx, RequestHandler& next)
{
    auto result = OnRequest(ctx);
    if (!result.IsNone())
        return result;
    return next.Handle(ctx);
}

// ── MiddlewareChain ──

void MiddlewareChain::Add(std::unique_ptr<Middleware> mw)
{
    middlewares_.push_back(std::move(mw));
}

// 原始字节阶段：遍历中间件，任一有效即短路
Response MiddlewareChain::ProcessRaw(const char* data, size_t len)
{
    for (auto& mw : middlewares_) {
        auto resp = mw->OnRawData(data, len);
        if (!resp.IsNone())
            return resp;
    }
    return Response::None();
}

// 解析完成阶段：洋葱链 → handler
Response MiddlewareChain::Execute(const Context& ctx,
                                     RequestHandler& final)
{
    return ExecuteFrom(0, ctx, final);
}

Response MiddlewareChain::ExecuteFrom(size_t index,
                                         const Context& ctx,
                                         RequestHandler& final)
{
    if (index >= middlewares_.size())
        return final.Handle(ctx);

    struct ChainLink : RequestHandler {
        MiddlewareChain& chain;
        size_t next_idx;
        RequestHandler& final_ref;

        ChainLink(MiddlewareChain& ch, size_t idx, RequestHandler& fin)
            : chain(ch), next_idx(idx), final_ref(fin) {}

        Response Handle(const Context& c) override
        {
            return chain.ExecuteFrom(next_idx, c, final_ref);
        }
    };

    ChainLink next(*this, index + 1, final);
    return middlewares_[index]->Handle(ctx, next);
}

// ── LoggingMiddleware ──

Response LoggingMiddleware::Handle(const Context& ctx,
                                       RequestHandler& next)
{
    FastLogger::Instance().Log(
        std::string(ctx.Method()) + " " + std::string(ctx.Path()));
    return next.Handle(ctx);
}

// ── CORSMiddleware ──

Response CORSMiddleware::Handle(const Context& ctx,
                                    RequestHandler& next)
{
    if (ctx.Method() == "OPTIONS") {
        auto r = Response::Raw(204,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
        return r;
    }

    ctx.AddResponseHeader("Access-Control-Allow-Origin", "*");
    return next.Handle(ctx);
}

// ── MetricsMiddleware ──

Response MetricsMiddleware::Handle(const Context& ctx,
                                       RequestHandler& next)
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

    // Redirect /dashboard → /dashboard/ so StaticFileHandler resolves index.html
    if (path == "/dashboard")
    {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(301, "HTTP/1.1 301 Moved Permanently\r\nLocation: /dashboard/\r\nContent-Length: 0\r\n\r\n");
        Response resp(301, *pool);
        resp.Header("Location", "/dashboard/");
        resp.EndHeaders();
        return resp;
    }

    if (path == "/metrics/stream" || path == "/metrics/stream/")
    {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(200, "data: {\"error\":\"no pool\"}\n\n");

        // Parse push interval from query, e.g. /metrics/stream?interval=2000
        int interval_ms = kDefaultPushMs;
        // H1Parser doesn't expose query params directly,
        // so we accept the convention via the path suffix.
        // For now, just use the default — the client's EventSource
        // respects the server's push rate via the retry directive.

        return Response::SSEStream(*pool, interval_ms);
    }

    return next.Handle(ctx);
}

