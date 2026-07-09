#include "db/connection_pool.hpp"
#include <iostream>
#include <mutex>

ConnectionPool::ConnectionPool(const std::string& db_path,
                               asio::thread_pool& thread_pool,
                               size_t max_connections)
    : thread_pool_(thread_pool)
    , db_path_(db_path)
{
    for (size_t i = 0; i < max_connections; ++i) {
        auto db = std::make_unique<Database>(db_path_, thread_pool_);
        available_.push(db.get());
        all_.push_back(std::move(db));
    }
    std::cout << "[pool] " << max_connections << " 个连接已就绪" << std::endl;
}

asio::awaitable<DbConnection*> ConnectionPool::Acquire()
{
    auto exec = co_await asio::this_coro::executor;
    auto timer = asio::steady_timer(exec);
    asio::error_code ec;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!available_.empty()) {
                auto* db = available_.front();
                available_.pop();
                co_return db;
            }
        }
        // 无可用连接，等待 50ms 后重试
        timer.expires_after(std::chrono::milliseconds(50));
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
}

void ConnectionPool::Release(DbConnection* db)
{
    std::lock_guard<std::mutex> lock(mutex_);
    available_.push(static_cast<Database*>(db));
}
