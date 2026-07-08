#include "rpc/retry_policy.hpp"
#include <algorithm>

RetryPolicy::RetryAction RetryPolicy::DecideRetry(
    int status_code, int retry_count, int max_retries) {

    if (retry_count >= max_retries) {
        return RetryAction::FAIL;
    }

    // 500: 实例宕机，立即切换实例
    if (status_code == 500) {
        return RetryAction::RETRY_OTHER;
    }

    // 300: 实例高负荷，等待后重试同实例
    if (status_code == 300) {
        return RetryAction::RETRY_SAME;
    }

    // 其他状态码：失败
    return RetryAction::FAIL;
}

int RetryPolicy::GetBackoffDelayMs(int retry_count) {
    // 指数退避: 100ms, 200ms, 400ms, ...
    int delay = 100 << retry_count;
    // 上限 5 秒
    return std::min(delay, 5000);
}
