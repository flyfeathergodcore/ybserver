#pragma once
#include <string>

// 前向声明
class MetricsCollector;

// SSE 推送初始载荷
std::string SseInitialPayload(MetricsCollector* metrics);

// SSE 推送状态
struct SsePushState {
    void Init(MetricsCollector* metrics);
    std::string BuildPayload(MetricsCollector* metrics);

private:
    MetricsCollector* metrics_ = nullptr;
};
