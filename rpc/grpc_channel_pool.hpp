#pragma once
#include "rpc/grpc_bridge.hpp"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════
// GrpcChannelPool — gRPC Channel 连接池
//
// 按 target (host:port) 缓存 gRPC Channel。
// Channel 是线程安全的，多个 worker 可共享同一个。
// ═══════════════════════════════════════════════════════════════════

class GrpcChannelPool {
public:
    explicit GrpcChannelPool(std::shared_ptr<GrpcBridge> bridge);

    std::shared_ptr<grpc::Channel> GetChannel(const std::string& target);

    std::shared_ptr<GrpcBridge> Bridge() const { return bridge_; }

private:
    std::shared_ptr<GrpcBridge> bridge_;
    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> channels_;
};
