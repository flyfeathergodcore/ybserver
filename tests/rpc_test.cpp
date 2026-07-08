#include "rpc/service_discovery.hpp"
#include "rpc/load_balancer.hpp"
#include "rpc/retry_policy.hpp"
#include <cassert>
#include <iostream>

void TestLoadBalancer() {
    LoadBalancer lb;

    // 创建测试实例
    std::vector<rpc::ServiceInstance> instances = {
        {"ai-chat", "127.0.0.1:50051", 1, true, 0},
        {"ai-chat", "127.0.0.1:50052", 1, true, 0}
    };

    // 第一次选择应该返回第一个实例（都是 0 连接）
    auto selected = lb.SelectInstance(instances);
    assert(selected.has_value());
    assert(selected->address == "127.0.0.1:50051");

    // 记录第一个实例打开一个连接
    lb.RecordConnectionOpen("127.0.0.1:50051");

    // 再次选择应该返回第二个实例（连接数更少）
    selected = lb.SelectInstance(instances);
    assert(selected.has_value());
    assert(selected->address == "127.0.0.1:50052");

    std::cout << "[test] LoadBalancer test passed" << std::endl;
}

void TestRetryPolicy() {
    RetryPolicy policy;

    // 500 应该返回 RETRY_OTHER
    auto action = policy.DecideRetry(500, 0, 3);
    assert(action == RetryPolicy::RetryAction::RETRY_OTHER);

    // 300 应该返回 RETRY_SAME
    action = policy.DecideRetry(300, 0, 3);
    assert(action == RetryPolicy::RetryAction::RETRY_SAME);

    // 超过最大重试次数应该返回 FAIL
    action = policy.DecideRetry(500, 3, 3);
    assert(action == RetryPolicy::RetryAction::FAIL);

    // 测试退避延迟
    int delay0 = policy.GetBackoffDelayMs(0);  // 100ms
    int delay1 = policy.GetBackoffDelayMs(1);  // 200ms
    assert(delay0 == 100);
    assert(delay1 == 200);

    std::cout << "[test] RetryPolicy test passed" << std::endl;
}

int main() {
    try {
        TestLoadBalancer();
        TestRetryPolicy();
        std::cout << "[test] All RPC module tests passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[error] Test failed: " << e.what() << std::endl;
        return 1;
    }
}
