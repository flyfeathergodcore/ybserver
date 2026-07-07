#include "rpc/grpc_channel_pool.hpp"

GrpcChannelPool::GrpcChannelPool(std::shared_ptr<GrpcBridge> bridge)
    : bridge_(std::move(bridge))
{}

std::shared_ptr<grpc::Channel> GrpcChannelPool::GetChannel(
    const std::string& target)
{
    auto it = channels_.find(target);
    if (it != channels_.end())
        return it->second;

    auto ch = bridge_->CreateChannel(target);
    channels_[target] = ch;
    return ch;
}
