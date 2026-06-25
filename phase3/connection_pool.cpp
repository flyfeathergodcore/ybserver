#include "connection_pool.hpp"
#include <iostream>

ConnectionPool::ConnectionPool(const std::string& db_path,
                               asio::thread_pool& thread_pool,
                               size_t max_connections)
    : thread_pool_(thread_pool)
    , db_path_(db_path)
    , strand_(asio::make_strand(thread_pool.get_executor()))
{
    for (size_t i = 0; i < max_connections; ++i) {
        auto db = std::make_unique<Database>(db_path_, thread_pool_);
        available_.push(db.get());
        all_.push_back(std::move(db));
    }
    std::cout << "[pool] " << max_connections << " 个连接已就绪" << std::endl;
}

asio::awaitable<Database*> ConnectionPool::Acquire()
{
    // 在 strand 上执行，保证线程安全
    auto exec = co_await asio::this_coro::executor;

    while (available_.empty()) {
        // 没有可用连接：挂起，等 Release 唤醒
        co_await asio::post(strand_, asio::use_awaitable);
    }

    // 有可用连接了
    auto* db = available_.front();
    available_.pop();

    co_return db;
}

void ConnectionPool::Release(Database* db)
{
    available_.push(db);
    NotifyOne();
}

void ConnectionPool::NotifyOne()
{
    asio::post(strand_, [this] {
        // 唤醒一个等待者
    });
}
