#include "rpc/load_balancer.hpp"
#include <chrono>
#include <algorithm>

std::optional<rpc::ServiceInstance> LoadBalancer::SelectInstance(
    const std::vector<rpc::ServiceInstance>& instances) {
    if (instances.empty()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(lock_);

    // 过滤健康实例
    std::vector<const rpc::ServiceInstance*> healthy;
    for (const auto& inst : instances) {
        if (inst.healthy && IsHealthy(inst.address)) {
            healthy.push_back(&inst);
        }
    }

    if (healthy.empty()) {
        return std::nullopt;
    }

    // 选择连接数最少的实例
    const rpc::ServiceInstance* best = healthy[0];
    int best_count = connection_count_[best->address];

    for (const auto* inst : healthy) {
        int count = connection_count_[inst->address];
        if (count < best_count) {
            best = inst;
            best_count = count;
        }
    }

    return *best;
}

void LoadBalancer::RecordConnectionOpen(const std::string& addr) {
    std::lock_guard<std::mutex> lock(lock_);
    connection_count_[addr]++;
}

void LoadBalancer::RecordConnectionClose(const std::string& addr) {
    std::lock_guard<std::mutex> lock(lock_);
    if (connection_count_[addr] > 0) {
        connection_count_[addr]--;
    }
}

void LoadBalancer::MarkUnhealthy(const std::string& addr, int timeout_sec) {
    std::lock_guard<std::mutex> lock(lock_);
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    unhealthy_until_ms_[addr] = now + timeout_sec * 1000;
}

bool LoadBalancer::IsHealthy(const std::string& addr) const {
    auto it = unhealthy_until_ms_.find(addr);
    if (it == unhealthy_until_ms_.end()) {
        return true;
    }
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return now > it->second;
}
