// ─────────────────────────────────────────────────────────────
// Phase 3 — 协程版 SQLite 连接池
//
// 管理多个 Database 连接，支持协程版 acquire/release。
// 当所有连接被占用时，协程挂起等待。
// ─────────────────────────────────────────────────────────────

#ifndef CONNECTION_POOL_HPP
#define CONNECTION_POOL_HPP

#include "database.hpp"
#include <asio.hpp>
#include <memory>
#include <queue>

class ConnectionPool {
public:
    ConnectionPool(const std::string& db_path,
                   asio::thread_pool& thread_pool,
                   size_t max_connections = 4);

    // 获取一个数据库连接（如果没有可用连接则挂起等待）
    asio::awaitable<Database*> Acquire();

    // 归还一个数据库连接
    void Release(Database* db);

private:
    std::vector<std::unique_ptr<Database>> all_;
    std::queue<Database*> available_;
    asio::thread_pool& thread_pool_;
    std::string db_path_;

    // 等待队列：挂起的协程
    asio::io_context::strand strand_;
    std::deque<asio::any_io_executor> waiters_;
    void NotifyOne();
};

#endif
