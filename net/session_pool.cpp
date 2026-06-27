#include "net/session_pool.hpp"
#include <iostream>

// 尝试获取空闲 Session（empty shell，需 Reset 注入新 stream+parser）
std::shared_ptr<Session<asio::ssl::stream<asio::ip::tcp::socket>>>
SessionPool::TryAcquireSession()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (!idle_.empty()) {
        auto s = std::move(idle_.back());
        idle_.pop_back();
        return s;
    }
    return nullptr;
}

void SessionPool::ReleaseSession(
    std::shared_ptr<Session<asio::ssl::stream<asio::ip::tcp::socket>>> session)
{
    std::lock_guard<std::mutex> lock(mtx_);
    idle_.push_back(std::move(session));
}

size_t SessionPool::IdleCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return idle_.size();
}
