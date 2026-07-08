#pragma once
#include <chrono>

class RetryPolicy {
public:
    enum class RetryAction {
        SUCCESS,       // 成功，不重试
        RETRY_SAME,    // 重试同实例
        RETRY_OTHER,   // 切换实例重试
        FAIL           // 放弃
    };

    // 根据状态码和重试次数决策
    RetryAction DecideRetry(int status_code, int retry_count, int max_retries);

    // 获取退避延迟（毫秒）
    int GetBackoffDelayMs(int retry_count);
};
