#include "rpc/rpc_module.hpp"
#include <iostream>

bool RpcModule::Initialize(const std::string& consul_addr) {
    discovery_ = std::make_unique<ServiceDiscovery>();
    balancer_ = std::make_unique<LoadBalancer>();
    retry_policy_ = std::make_unique<RetryPolicy>();

    if (!discovery_->Connect(consul_addr)) {
        std::cerr << "[rpc] Failed to connect Consul" << std::endl;
        return false;
    }

    // 监听三个服务
    discovery_->WatchService("ai-chat");
    discovery_->WatchService("rag");
    discovery_->WatchService("ppt");

    std::cout << "[rpc] RpcModule initialized" << std::endl;
    return true;
}

std::shared_ptr<grpc::Channel> RpcModule::GetChannel(
    const std::string& service_name) {

    // 从 ServiceDiscovery 获取活跃实例
    auto instances = discovery_->GetInstances(service_name);

    // 通过 LoadBalancer 选择最优实例
    auto selected = balancer_->SelectInstance(instances);
    if (!selected) {
        std::cerr << "[rpc] No available instance for " << service_name << std::endl;
        return nullptr;
    }

    // 从 GrpcChannelPool 获取或创建 Channel
    return channel_pool_->GetChannel(selected->address);
}
