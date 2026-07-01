#include "handler/health.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include <ctime>

Response HealthHandler::Handle(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool)
        return Response::Raw(200, R"({"status":"ok"})");

    static const auto start_time = std::time(nullptr);
    auto now = std::time(nullptr);
    auto uptime = static_cast<unsigned long>(now - start_time);

    // Minimal JSON — suitable for both liveness & readiness probes
    std::string json = R"({"status":"ok","uptime":)";
    json += std::to_string(uptime);
    json += "}";

    Response resp(200, *pool);
    resp.Header("Content-Type", "application/json");
    resp.Header("Content-Length", json.size());
    resp.Header("Cache-Control", "no-cache");
    for (int i = 0; i < ctx.ResponseHeaderCount(); i++)
        resp.Header(ctx.ResponseHeaderKey(i), ctx.ResponseHeaderVal(i));
    resp.EndHeaders();
    pool->Write({json.data(), json.size()});
    return resp;
}
