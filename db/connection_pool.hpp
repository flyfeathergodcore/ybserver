#pragma once
#include "db/db_interface.hpp"
#include "db/database.hpp"
#include <asio.hpp>
#include <memory>
#include <queue>
#include <deque>

// Manages a pool of Database connections with coroutine-friendly
// acquire/release semantics. When all connections are busy,
// Acquire suspends until one becomes available.
class ConnectionPool : public DbPool {
public:
    ConnectionPool(const std::string& db_path,
                   asio::thread_pool& thread_pool,
                   size_t max_connections = 4);

    // ── DbPool ──
    asio::awaitable<DbConnection*> Acquire() override;
    void Release(DbConnection* db) override;

private:
    std::vector<std::unique_ptr<Database>> all_;
    std::queue<Database*> available_;
    asio::thread_pool& thread_pool_;
    std::string db_path_;

    asio::strand<asio::thread_pool::executor_type> strand_;
    std::deque<asio::any_io_executor> waiters_;
    void NotifyOne();
};
