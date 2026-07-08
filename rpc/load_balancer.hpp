#pragma once
#include "rpc/types.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <cstdint>

class LoadBalancer {
public:
    // 根据最少连接选择实例
    std::optional<rpc::ServiceInstance> SelectInstance(
        const std::vector<rpc::ServiceInstance>& instances);

    // 记录连接打开
    void RecordConnectionOpen(const std::string& addr);

    // 记录连接关闭
    void RecordConnectionClose(const std::string& addr);

    // 标记实例不健康（隔离 timeout_sec 秒）
    void MarkUnhealthy(const std::string& addr, int timeout_sec = 30);

private:
    std::map<std::string, int> connection_count_;
    std::map<std::string, int64_t> unhealthy_until_ms_;
    mutable std::mutex lock_;

    bool IsHealthy(const std::string& addr) const;
};
