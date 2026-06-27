#include "rpc/connection_pool.hpp"
#include <iostream>

ConnectionPool::ConnectionPool(const std::string& db_path,
                               asio::thread_pool& thread_pool,
                               size_t max_connections)
    : thread_pool_(thread_pool)
    , db_path_(db_path)
    , strand_(thread_pool.get_executor())
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
    auto exec = co_await asio::this_coro::executor;

    while (available_.empty()) {
        co_await asio::post(strand_, asio::use_awaitable);
    }

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
        // Wake a waiter
    });
}
