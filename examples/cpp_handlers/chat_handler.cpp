#include "examples/cpp_handlers/chat_handler.hpp"
#include "examples/proto/chat.pb.h"
#include "examples/proto/chat.grpc.pb.h"
#include <agrpc/asio_grpc.hpp>
#include <nlohmann/json.hpp>
#include "net/object_pool.hpp"

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

    // ── 提取字段（引用 JSON 内部字符串，无拷贝；数值直接读取）──
    static const std::string kDefaultId      = "default";
    static const std::string kDefaultMessage = "";
    static const std::string kDefaultModel   = "qwen";

    const auto& id      = body.contains("id")      ? body["id"].get_ref<const std::string&>()      : kDefaultId;
    const auto& message = body.contains("message")  ? body["message"].get_ref<const std::string&>() : kDefaultMessage;
    const auto& model   = body.contains("model")    ? body["model"].get_ref<const std::string&>()   : kDefaultModel;
    const double temperature = body.value("temperature", 0.7);
    const int    max_tokens  = body.value("max_tokens", 2048);

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

    // ── 从对象池取 ChatClientMessage（复用，避免反复 malloc）──
    // 进程级静态池，所有并发请求共享，容量 32 个
    static ObjectPool<ai::chat::ChatClientMessage, 32> msg_pool;
    auto req_msg = msg_pool.Acquire();  // 离开作用域自动 Clear() 归还

    // ── 构造 gRPC 请求 ──
    grpc::ClientContext grpc_ctx;
    auto [reader, writer] = stub->ChatStream(&grpc_ctx);

    auto* req = req_msg->mutable_chat_request();
    req->set_session_id(id);
    req->set_user_message(message);
    auto* cfg = req->mutable_config();
    cfg->set_model(model);
    cfg->set_temperature(temperature);
    cfg->set_max_tokens(max_tokens);

    bool ok = co_await agrpc::write(*writer, *req_msg);
    if (!ok) { sink.End(); co_return; }

    // ── 流式读取并推送 SSE ──
    // reply 也从对象池复用
    static ObjectPool<ai::chat::ChatServerMessage, 32> reply_pool;
    auto reply = reply_pool.Acquire();

    while (co_await agrpc::read(*reader, *reply)) {
        switch (reply->event_case()) {
        case ai::chat::ChatServerMessage::kToken:
            if (!co_await sink.PushSSE(reply->token()))
                goto cancel;
            break;
        case ai::chat::ChatServerMessage::kFinish:
            co_await sink.PushSSE("[DONE]");
            sink.End();
            co_return;
        case ai::chat::ChatServerMessage::kError:
            co_await sink.PushSSE("[ERROR: " + reply->error() + "]");
            sink.End();
            co_return;
        default:
            break;
        }
        reply->Clear();
    }

    sink.End();
    co_return;

cancel:
    // 从池取消息对象发取消信号
    static ObjectPool<ai::chat::ChatClientMessage, 8> cancel_pool;
    auto cancel_msg = cancel_pool.Acquire();
    cancel_msg->mutable_cancel();
    co_await agrpc::write(*writer, *cancel_msg);
}

