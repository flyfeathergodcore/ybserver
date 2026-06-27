#pragma once
#include "net/session.hpp"
#include "http/llhttp_parser.hpp"
#include <memory>
#include <mutex>
#include <vector>

// ── Session 对象池 ──
//
// 避免每条新连接反复 make_shared 的开销。
// 池里存空闲的 shared_ptr<Session> 壳，
// Server 取出 → Reset(新stream, 新parser) → co_spawn → 用完归还。
//
class SessionPool {
public:
    SessionPool() = default;

    // 取一个空闲 Session（空壳，需 Reset），池空返回 nullptr
    std::shared_ptr<Session<asio::ssl::stream<asio::ip::tcp::socket>>>
    TryAcquireSession();

    // 归还 Session
    void ReleaseSession(
        std::shared_ptr<Session<asio::ssl::stream<asio::ip::tcp::socket>>>);

    size_t IdleCount() const;

private:
    mutable std::mutex mtx_;
    std::vector<
        std::shared_ptr<Session<asio::ssl::stream<asio::ip::tcp::socket>>>>
        idle_;
};
