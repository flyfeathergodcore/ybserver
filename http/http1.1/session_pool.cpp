#include "http/http1.1/session_pool.hpp"

std::shared_ptr<H11Session<asio::ssl::stream<asio::ip::tcp::socket>>>
H11SessionPool::TryAcquireSession()
{
    if (!idle_.empty()) {
        auto s = std::move(idle_.back());
        idle_.pop_back();
        return s;
    }
    return nullptr;
}

void H11SessionPool::ReleaseSession(
    std::shared_ptr<H11Session<asio::ssl::stream<asio::ip::tcp::socket>>> session)
{
    idle_.push_back(std::move(session));
}

size_t H11SessionPool::IdleCount() const
{
    return idle_.size();
}
