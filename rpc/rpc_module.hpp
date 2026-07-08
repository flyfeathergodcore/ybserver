#pragma once
#include "rpc/service_discovery.hpp"
#include "rpc/load_balancer.hpp"
#include "rpc/retry_policy.hpp"
#include "rpc/types.hpp"
#include "rpc/grpc_channel_pool.hpp"
#include <memory>
#include <string>
#include <grpcpp/grpcpp.h>

class RpcModule {
public:
    static RpcModule& Instance() {
        static RpcModule instance;
        return instance;
    }

    // 禁止拷贝和移动
    RpcModule(const RpcModule&) = delete;
    RpcModule& operator=(const RpcModule&) = delete;

    // 初始化 RPC 模块
    bool Initialize(const std::string& consul_addr);

    // 获取到服务的 gRPC Channel
    std::shared_ptr<grpc::Channel> GetChannel(
        const std::string& service_name);

private:
    RpcModule() = default;

    std::unique_ptr<ServiceDiscovery> discovery_;
    std::unique_ptr<LoadBalancer> balancer_;
    std::unique_ptr<RetryPolicy> retry_policy_;
    std::shared_ptr<GrpcChannelPool> channel_pool_;
};
