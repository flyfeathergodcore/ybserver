#include "examples/cpp_handlers/chat_handler.hpp"
#include "examples/cpp_handlers/auth_util.hpp"
#include "log/logger.hpp"
#include "examples/proto/chat.pb.h"
#include "examples/proto/chat.grpc.pb.h"
#include "handler/router.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>

ChatHandler::ChatHandler(std::shared_ptr<GrpcChannelPool> pool)
    : pool_(std::move(pool))
{}

asio::awaitable<void> ChatHandler::HandleStream(
    const Context& ctx, StreamSink& sink)
{
    // ── 解析 JSON body ──
    nlohmann::json body;
    std::string json_error;
    try {
        body = nlohmann::json::parse(ctx.Body());
    } catch (const nlohmann::json::exception& e) {
        json_error = e.what();
    }
    if (!json_error.empty()) {
        co_await sink.PushSSE(R"({"error":"invalid json: )" + json_error + "\"}");
        sink.End();
        co_return;
    }

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

    // ── Token 鉴权 ──
    auto auth = ctx.Header("authorization");
    std::string token_str;
    static const std::string prefix = "Bearer ";
    if (auth.substr(0, prefix.size()) == prefix)
        token_str = std::string(auth.substr(prefix.size()));
    std::string user = token_str.empty() ? "" : auth_verify_token(token_str);
    if (user.empty()) {
        Logger::Instance().Business("CHAT", "auth_fail",
            "token=" + token_str.substr(0, 16) + "...");
        co_await sink.PushSSE(R"({"error":"unauthorized","code":401})");
        sink.End();
        co_return;
    }

    Logger::Instance().Business("CHAT", "chat",
        "user=" + user + " msg_len=" + std::to_string(message.size()));

    // ── 通过注入的 GrpcChannelPool 获取 gRPC Channel ──
    // Handler 不直接创建 gRPC 连接，而是使用框架提供的通道池，
    // 与服务发现和负载均衡集成。
    auto channel = pool_->GetChannel("ai-chat:50051");
    if (!channel) {
        co_await sink.PushSSE(R"({"error":"cannot get channel to ai-chat:50051"})");
        sink.End();
        co_return;
    }
    auto stub = ai::chat::ChatService::NewStub(channel);

    ai::chat::ChatClientMessage req_msg;
    auto* req = req_msg.mutable_chat_request();
    req->set_session_id(id);
    req->set_user_message(message);
    auto* cfg = req->mutable_config();
    cfg->set_model(model);
    cfg->set_temperature(temperature);
    cfg->set_max_tokens(max_tokens);

    std::vector<std::string> tokens;
    std::string grpc_error;

    {
        grpc::ClientContext context;
        auto rw = stub->ChatStream(&context);
        if (!rw->Write(req_msg)) {
            grpc_error = "rpc write failed";
        } else {
            rw->WritesDone();
            ai::chat::ChatServerMessage reply;
            while (rw->Read(&reply)) {
                switch (reply.event_case()) {
                case ai::chat::ChatServerMessage::kToken:
                    tokens.push_back(std::move(*reply.mutable_token()));
                    break;
                case ai::chat::ChatServerMessage::kFinish:
                    tokens.emplace_back("[DONE]");
                    break;
                case ai::chat::ChatServerMessage::kError:
                    grpc_error = reply.error();
                    break;
                default: break;
                }
                reply.Clear();
            }
        }
        rw->Finish();
    }

    if (!grpc_error.empty()) {
        co_await sink.PushSSE("[ERROR: " + grpc_error + "]");
    } else {
        for (const auto& t : tokens) {
            if (t == "[DONE]") { co_await sink.PushSSE(t); break; }
            if (!co_await sink.PushSSE(t)) break;
        }
    }
    sink.End();
}

extern "C" void register_routes(Router& router)
{
    static auto bridge = std::make_shared<GrpcBridge>(asio::system_executor());
    static auto pool = std::make_shared<GrpcChannelPool>(bridge);
    router.Post("/v1/chat", std::make_unique<ChatHandler>(pool));
    std::cout << "[route] POST /v1/chat → ChatHandler (hot-plug)" << std::endl;
}
