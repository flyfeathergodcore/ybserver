#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace rpc {

// 服务实例信息
struct ServiceInstance {
    std::string service_name;
    std::string address;
    int weight = 1;
    bool healthy = true;
    int64_t last_heartbeat_ms = 0;
};

// RPC 错误码
enum class RpcErrorCode {
    SUCCESS = 0,
    SERVICE_UNAVAILABLE = 1,
    TIMEOUT = 2,
    INVALID_ARGUMENT = 3,
    INTERNAL_ERROR = 4,
    UNKNOWN = 5
};

// RPC 错误
struct RpcError {
    RpcErrorCode code = RpcErrorCode::UNKNOWN;
    std::string message;

    bool ok() const { return code == RpcErrorCode::SUCCESS; }
};

// RPC 调用选项
struct RpcCallOptions {
    std::string service_name;
    std::chrono::milliseconds timeout{30000};
    int max_retries = 3;
};

}
