/**
 * shopping_handler — 购物导购 handler
 *
 * 通过 gRPC 调用 Python shopping-agent 后端（ai.shopping.ShoppingService）。
 *
 * POST /shopping/api/chat  — 流式对话（SSE）
 * POST /shopping/api/reset — 重置会话
 */
#include "handler/request_handler.hpp"
#include "handler/router.hpp"
#include "rpc/grpc_bridge.hpp"
#include "rpc/grpc_channel_pool.hpp"
#include "examples/proto/shopping.pb.h"
#include "examples/proto/shopping.grpc.pb.h"
#include "net/response.hpp"
#include "http/context.hpp"
#include "net/session_region.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <iostream>
#include <chrono>

using json = nlohmann::json;

// ═══════════════════════════════════════════════════════════════════
// ShoppingHandler — 通过 gRPC 调用 Python shopping-agent 后端
//
// 流式 Handler：
//   1. 接收 HTTP POST /shopping/api/chat {message, session_id}
//   2. 通过 gRPC Client 调 Python 的 ChatStream RPC
//   3. 收集所有事件，通过 SSE 推回客户端
// ═══════════════════════════════════════════════════════════════════

class ShoppingHandler : public RequestHandler {
public:
    explicit ShoppingHandler(std::shared_ptr<GrpcChannelPool> pool)
        : pool_(std::move(pool))
    {}

    bool IsStream() const override { return true; }
    Response Handle(const Context& ctx) override {
        return Response::Error(400, *ctx.Pool());
    }

    asio::awaitable<void> HandleStream(const Context& ctx,
                                        StreamSink& sink) override;

private:
    std::shared_ptr<GrpcChannelPool> pool_;
};

asio::awaitable<void> ShoppingHandler::HandleStream(
    const Context& ctx, StreamSink& sink)
{
    // ── 解析 JSON body ──
    json body;
    std::string json_error;
    try {
        body = json::parse(ctx.Body());
    } catch (const json::exception& e) {
        json_error = e.what();
    }
    if (!json_error.empty()) {
        co_await sink.PushSSE(R"({"error":"invalid json: )" + json_error + "\"}");
        sink.End();
        co_return;
    }

    static const std::string kDefaultSid = "";
    static const std::string kDefaultMsg = "";
    static const std::string kDefaultUid = "";
    const auto& sid   = body.contains("session_id") ? body["session_id"].get_ref<const std::string&>() : kDefaultSid;
    const auto& msg   = body.contains("message")     ? body["message"].get_ref<const std::string&>()    : kDefaultMsg;
    const auto& uid   = body.contains("user_id")     ? body["user_id"].get_ref<const std::string&>()    : kDefaultUid;

    if (msg.empty()) {
        co_await sink.PushSSE(R"({"error":"message is required"})");
        sink.End();
        co_return;
    }

    // ── 通过 GrpcChannelPool 获取 gRPC Channel ──
    // 连接到 Python shopping-agent 的 gRPC 端口（docker 网络内）
    auto channel = pool_->GetChannel("shopping-agent:50054");
    if (!channel) {
        co_await sink.PushSSE(R"({"error":"cannot get channel to shopping-agent:50054"})");
        sink.End();
        co_return;
    }
    auto stub = ai::shopping::ShoppingService::NewStub(channel);

    ai::shopping::ShoppingRequest req;
    req.set_session_id(sid);
    req.set_message(msg);
    req.set_user_id(uid);

    std::vector<std::string> sse_events;
    std::string grpc_error;

    // ── 日志：gRPC 请求 → Python 后端 ──
    std::cerr << "[grpc] → shopping-agent:50054  ChatStream"
              << "  sid=" << (sid.empty() ? "new" : sid.substr(0, 20))
              << "  uid=" << (uid.empty() ? "none" : uid.substr(0, 20))
              << "  msg=" << msg.substr(0, 40) << "..." << std::endl;
    auto grpc_t0 = std::chrono::steady_clock::now();

    {
        grpc::ClientContext context;
        auto rw = stub->ChatStream(&context, req);
        ai::shopping::ShoppingEvent event;
        while (rw->Read(&event)) {
            switch (event.event_case()) {
            case ai::shopping::ShoppingEvent::kMeta: {
                const auto& m = event.meta();
                json j = {
                    {"session_id", m.session_id()},
                    {"ready",      m.ready()},
                };
                sse_events.push_back(j.dump());
                break;
            }
            case ai::shopping::ShoppingEvent::kToken: {
                json j = {{"token", event.token().token()}};
                sse_events.push_back(j.dump());
                break;
            }
            case ai::shopping::ShoppingEvent::kDone: {
                const auto& d = event.done();
                json j = {
                    {"done",      true},
                    {"full_text", d.full_text()},
                    {"stage",     d.stage()},
                    {"ready",     d.ready()},
                };
                if (d.candidates_size() > 0) {
                    json cands = json::array();
                    for (const auto& c : d.candidates()) {
                        cands.push_back({
                            {"name",   c.name()},
                            {"price",  c.price()},
                            {"rating", c.rating()},
                        });
                    }
                    j["candidates"] = std::move(cands);
                }
                sse_events.push_back(j.dump());
                break;
            }
            case ai::shopping::ShoppingEvent::kError: {
                grpc_error = event.error().message();
                break;
            }
            default:
                break;
            }
            event.Clear();
        }
        auto status = rw->Finish();
        if (!status.ok() && grpc_error.empty()) {
            grpc_error = status.error_message();
        }
    }

    auto grpc_t1 = std::chrono::steady_clock::now();
    auto grpc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(grpc_t1 - grpc_t0).count();
    if (!grpc_error.empty()) {
        std::cerr << "[grpc] ← ERROR  " << grpc_ms << "ms  " << grpc_error << std::endl;
    } else {
        std::cerr << "[grpc] ← OK  " << grpc_ms << "ms  "
                  << sse_events.size() << " events"
                  << "  first=\"" << (sse_events.empty() ? "" : sse_events[0].substr(0, 60)) << "\"" << std::endl;
    }

    if (!grpc_error.empty()) {
        json err = {{"error", grpc_error}};
        co_await sink.PushSSE(err.dump());
    } else {
        for (const auto& ev : sse_events) {
            if (!co_await sink.PushSSE(ev)) break;
        }
    }
    sink.End();
}

// ═══════════════════════════════════════════════════════════════════
// ResetHandler — 重置会话（一元 gRPC 调用）
// ═══════════════════════════════════════════════════════════════════

class ResetHandler : public RequestHandler {
public:
    explicit ResetHandler(std::shared_ptr<GrpcChannelPool> pool)
        : pool_(std::move(pool))
    {}

    Response Handle(const Context& ctx) override
    {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Error(500, *(SessionRegion*)nullptr);

        json body;
        try {
            body = json::parse(ctx.Body());
        } catch (...) {
            Response r(400, *pool);
            r.Body(R"({"error":"invalid json"})");
            r.EndHeaders();
            return r;
        }

        auto channel = pool_->GetChannel("shopping-agent:50054");
        if (!channel) {
            Response r(500, *pool);
            r.Body(R"({"error":"cannot get channel"})");
            r.EndHeaders();
            return r;
        }
        auto stub = ai::shopping::ShoppingService::NewStub(channel);

        ai::shopping::ResetRequest req;
        req.set_session_id(body.value("session_id", ""));

        ai::shopping::ResetResponse resp;
        grpc::ClientContext context;
        auto status = stub->ResetSession(&context, req, &resp);

        json result = {{"ok", resp.ok()}};
        std::string body_str = result.dump();

        Response r(status.ok() ? 200 : 500, *pool);
        r.Header("Content-Type", "application/json; charset=utf-8");
        r.Header("Access-Control-Allow-Origin", "*");
        r.Body(body_str);
        r.EndHeaders();
        return r;
    }

private:
    std::shared_ptr<GrpcChannelPool> pool_;
};

// ═══════════════════════════════════════════════════════════════════
// 热插拔插件入口
// ═══════════════════════════════════════════════════════════════════

extern "C" void register_routes(Router& router)
{
    static auto bridge = std::make_shared<GrpcBridge>(asio::system_executor());
    static auto pool   = std::make_shared<GrpcChannelPool>(bridge);
    router.Post("/shopping/api/chat",  std::make_unique<ShoppingHandler>(pool));
    router.Post("/shopping/api/reset", std::make_unique<ResetHandler>(pool));
    std::cout << "[shopping] handler @ /shopping/api/chat, /shopping/api/reset (gRPC)" << std::endl;
}
