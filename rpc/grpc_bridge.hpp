#pragma once
#include <asio.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// ═══════════════════════════════════════════════════════════════════
// GrpcBridge — gRPC CompletionQueue ↔ Asio io_context 桥接
//
// 管理一个 gRPC CompletionQueue，提供创建 Channel 的方法。
// agrpc 函数通过此 CQ 调度异步操作。
// ═══════════════════════════════════════════════════════════════════

class GrpcBridge : public std::enable_shared_from_this<GrpcBridge> {
public:
    explicit GrpcBridge(asio::any_io_executor executor);
    ~GrpcBridge();

    grpc::CompletionQueue& Queue() { return queue_; }

    std::shared_ptr<grpc::Channel> CreateChannel(
        const std::string& target,
        std::shared_ptr<grpc::ChannelCredentials> creds = nullptr);

private:
    grpc::CompletionQueue queue_;
};
