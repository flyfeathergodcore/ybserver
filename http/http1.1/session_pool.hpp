#pragma once
#include "http/http1.1/session.hpp"
#include <asio/ssl.hpp>
#include <memory>
#include <vector>

// ── H11Session 对象池 ──
//
// 避免每条新连接反复 make_shared 的开销。
// 池里存空闲的 shared_ptr<H11Session> 壳，
// Server 取出 → Reset(stream) → co_spawn → 用完归还。
//
class H11SessionPool {
public:
    H11SessionPool() = default;

    /// 取一个空闲 Session（空壳，需 Reset），池空返回 nullptr
    std::shared_ptr<H11Session<asio::ssl::stream<asio::ip::tcp::socket>>>
    TryAcquireSession();

    /// 归还 Session
    void ReleaseSession(
        std::shared_ptr<H11Session<asio::ssl::stream<asio::ip::tcp::socket>>>);

    size_t IdleCount() const;

private:
    std::vector<
        std::shared_ptr<H11Session<asio::ssl::stream<asio::ip::tcp::socket>>>>
        idle_;
};
