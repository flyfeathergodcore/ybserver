#include "rpc/grpc_bridge.hpp"

GrpcBridge::GrpcBridge(asio::any_io_executor executor)
{
    // agrpc 内部把 CompletionQueue 注册到 Asio executor
    (void)executor;
}

GrpcBridge::~GrpcBridge()
{
    queue_.Shutdown();
}

std::shared_ptr<grpc::Channel> GrpcBridge::CreateChannel(
    const std::string& target,
    std::shared_ptr<grpc::ChannelCredentials> creds)
{
    if (!creds)
        creds = grpc::InsecureChannelCredentials();
    return grpc::CreateChannel(target, creds);
}
