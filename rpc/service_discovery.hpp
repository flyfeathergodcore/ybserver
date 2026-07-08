#pragma once
#include "rpc/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>

class ServiceDiscovery {
public:
    ServiceDiscovery();
    ~ServiceDiscovery();

    // 连接 Consul
    bool Connect(const std::string& consul_addr);

    // 监听服务变更
    void WatchService(const std::string& service_name);

    // 获取活跃实例列表
    std::vector<rpc::ServiceInstance> GetInstances(
        const std::string& service_name);

private:
    std::string consul_addr_;
    std::map<std::string, std::vector<rpc::ServiceInstance>> instance_cache_;
    mutable std::mutex cache_lock_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> watch_threads_;

    // 后台心跳检测
    void HeartbeatCheck();

    // 从 Consul 查询服务实例
    std::vector<rpc::ServiceInstance> QueryConsul(
        const std::string& service_name);
};
