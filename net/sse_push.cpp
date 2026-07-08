#include "net/sse_push.hpp"
#include "handler/metrics.hpp"
#include <sstream>
#include <chrono>

std::string SseInitialPayload(MetricsCollector* metrics) {
    if (!metrics) {
        return "data: {\"error\":\"no metrics\"}\n\n";
    }

    std::ostringstream oss;
    oss << "data: {\"status\":\"connected\",\"workers\":"
        << metrics->WorkerCount() << "}\n\n";
    return oss.str();
}

void SsePushState::Init(MetricsCollector* metrics) {
    metrics_ = metrics;
}

std::string SsePushState::BuildPayload(MetricsCollector* metrics) {
    if (!metrics) {
        return "data: {\"error\":\"no metrics\"}\n\n";
    }

    // 简化实现：返回基本指标
    std::ostringstream oss;
    oss << "data: {\"timestamp\":" << std::chrono::system_clock::now().time_since_epoch().count()
        << ",\"workers\":" << metrics->WorkerCount()
        << "}\n\n";
    return oss.str();
}
