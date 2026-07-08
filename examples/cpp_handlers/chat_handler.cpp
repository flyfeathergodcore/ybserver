#include "examples/cpp_handlers/chat_handler.hpp"
#include "examples/proto/chat.pb.h"
#include "examples/proto/chat.grpc.pb.h"
#include <agrpc/asio_grpc.hpp>
#include <nlohmann/json.hpp>

ChatHandler::ChatHandler(std::shared_ptr<GrpcChannelPool> pool)
    : pool_(std::move(pool))
{}

// 请求 body JSON 格式：
// {
//   "id": "12345",
//   "message": "hello",
//   "model": "qwen",
//   "temperature": 0.7,
//   "max_tokens": 2048
// }

asio::awaitable<void> ChatHandler::HandleStream(
    const Context& ctx, StreamSink& sink)
{
    // ── 解析 JSON body ──
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(ctx.Body());
    } catch (const nlohmann::json::exception& e) {
        co_await sink.PushSSE(
            std::string(R"({"error":"invalid json: ")") + e.what() + "\"}");
        sink.End();
        co_return;
    }

    std::string id          = body.value("id", "default");
    std::string message     = body.value("message", "");
    std::string model       = body.value("model", "qwen");
    double      temperature = body.value("temperature", 0.7);
    int         max_tokens  = body.value("max_tokens", 2048);

    if (message.empty()) {
        co_await sink.PushSSE(R"({"error":"message is required"})");
        sink.End();
        co_return;
    }

    // ── 获取 gRPC Channel ──
    auto channel = pool_->GetChannel("ai-chat:50051");
    if (!channel) {
        co_await sink.PushSSE(R"({"error":"ai-chat service unavailable"})");
        sink.End();
        co_return;
    }

    auto stub = ai::chat::ChatService::NewStub(channel);

    // ── 构造 gRPC 请求 ──
    grpc::ClientContext grpc_ctx;
    auto [reader, writer] = stub->ChatStream(&grpc_ctx);

    ai::chat::ChatClientMessage req_msg;
    auto* req = req_msg.mutable_chat_request();
    req->set_session_id(id);
    req->set_user_message(message);
    auto* cfg = req->mutable_config();
    cfg->set_model(model);
    cfg->set_temperature(temperature);
    cfg->set_max_tokens(max_tokens);

    bool ok = co_await agrpc::write(*writer, req_msg);
    if (!ok) { sink.End(); co_return; }

    // ── 流式读取并推送 SSE ──
    ai::chat::ChatServerMessage reply;
    while (co_await agrpc::read(*reader, reply)) {
        switch (reply.event_case()) {
        case ai::chat::ChatServerMessage::kToken:
            if (!co_await sink.PushSSE(reply.token()))
                goto cancel;
            break;
        case ai::chat::ChatServerMessage::kFinish:
            co_await sink.PushSSE("[DONE]");
            sink.End();
            co_return;
        case ai::chat::ChatServerMessage::kError:
            co_await sink.PushSSE("[ERROR: " + reply.error() + "]");
            sink.End();
            co_return;
        default:
            break;
        }
        reply.Clear();
    }

    sink.End();
    co_return;

cancel:
    ai::chat::CancelSignal cancel;
    ai::chat::ChatClientMessage cancel_msg;
    *cancel_msg.mutable_cancel() = cancel;
    co_await agrpc::write(*writer, cancel_msg);
}
