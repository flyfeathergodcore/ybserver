#include "examples/cpp_handlers/chat_handler.hpp"
#include "examples/proto/chat.pb.h"
#include "examples/proto/chat.grpc.pb.h"
#include <agrpc/asio_grpc.hpp>

ChatHandler::ChatHandler(std::shared_ptr<GrpcChannelPool> pool)
    : pool_(std::move(pool))
{}

asio::awaitable<void> ChatHandler::HandleStream(
    const Context& ctx, StreamSink& sink)
{
    auto body = ctx.Body();
    auto channel = pool_->GetChannel("ai-chat:50051");
    auto stub = ai::chat::ChatService::NewStub(channel);

    grpc::ClientContext grpc_ctx;
    auto [reader, writer] = stub->ChatStream(&grpc_ctx);

    ai::chat::ChatClientMessage req_msg;
    req_msg.mutable_chat_request()->set_session_id("default");
    req_msg.mutable_chat_request()->set_user_message(body.data(), body.size());
    req_msg.mutable_chat_request()->mutable_config()->set_model("gpt-4o-mini");
    req_msg.mutable_chat_request()->mutable_config()->set_temperature(0.7);
    req_msg.mutable_chat_request()->mutable_config()->set_max_tokens(2048);

    bool ok = co_await agrpc::write(*writer, req_msg);
    if (!ok) { sink.End(); co_return; }

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
