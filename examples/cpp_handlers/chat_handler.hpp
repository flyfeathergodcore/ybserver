#pragma once
#include "handler/request_handler.hpp"
#include "rpc/grpc_bridge.hpp"
#include "rpc/grpc_channel_pool.hpp"
#include <memory>

// ═══════════════════════════════════════════════════════════════════
// ChatHandler — 通过 gRPC 调用 ai-chat 服务的 AI 对话 Handler
//
// 流式 Handler：
//   1. 接收 HTTP POST /v1/chat {user_message, session_id}
//   2. 通过 gRPC Client 调 ai-chat 的 ChatStream RPC
//   3. 逐 token 通过 SSE 推回客户端
// ═══════════════════════════════════════════════════════════════════

class ChatHandler : public RequestHandler {
public:
    explicit ChatHandler(std::shared_ptr<GrpcChannelPool> pool);

    bool IsStream() const override { return true; }
    Response Handle(const Context& ctx) override {
        return Response::Error(400, *ctx.Pool());
    }

    asio::awaitable<void> HandleStream(const Context& ctx,
                                        StreamSink& sink) override;

private:
    std::shared_ptr<GrpcChannelPool> pool_;
};
