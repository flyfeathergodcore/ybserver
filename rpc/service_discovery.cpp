#include "rpc/service_discovery.hpp"
#include <iostream>
#include <chrono>

ServiceDiscovery::ServiceDiscovery() = default;

ServiceDiscovery::~ServiceDiscovery() {
    running_ = false;
    for (auto& t : watch_threads_) {
        if (t.joinable()) t.join();
    }
}

bool ServiceDiscovery::Connect(const std::string& consul_addr) {
    consul_addr_ = consul_addr;
    running_ = true;
    std::cout << "[sd] 连接 Consul: " << consul_addr << std::endl;
    return true;
}

void ServiceDiscovery::WatchService(const std::string& service_name) {
    watch_threads_.emplace_back([this, service_name]() {
        while (running_) {
            auto instances = QueryConsul(service_name);
            {
                std::lock_guard<std::mutex> lock(cache_lock_);
                instance_cache_[service_name] = instances;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });
}

std::vector<rpc::ServiceInstance> ServiceDiscovery::GetInstances(
    const std::string& service_name) {
    std::lock_guard<std::mutex> lock(cache_lock_);
    auto it = instance_cache_.find(service_name);
    if (it != instance_cache_.end()) {
        return it->second;
    }
    return {};
}

std::vector<rpc::ServiceInstance> ServiceDiscovery::QueryConsul(
    const std::string& service_name) {
    // TODO: 实现真实的 HTTP 调用到 Consul API
    // 暂时返回硬编码实例用于测试
    std::vector<rpc::ServiceInstance> instances;
    if (service_name == "ai-chat") {
        instances.push_back({"ai-chat", "127.0.0.1:50051", 1, true, 0});
    } else if (service_name == "rag") {
        instances.push_back({"rag", "127.0.0.1:50052", 1, true, 0});
    } else if (service_name == "ppt") {
        instances.push_back({"ppt", "127.0.0.1:50053", 1, true, 0});
    }
    return instances;
}
