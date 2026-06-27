#include "rpc/database.hpp"
#include <iostream>
#include <cassert>
#include <cstdio>

asio::awaitable<void> Test(asio::io_context& ioc)
{
    asio::thread_pool db_pool(2);
    Database db("test.db", db_pool);

    co_await db.Execute(
        "CREATE TABLE IF NOT EXISTS requests ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  method TEXT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ")");
    std::cout << "[test] 表已创建" << std::endl;

    co_await db.Execute(
        "INSERT INTO requests (method, path) VALUES ('GET', '/')");
    auto id = co_await db.LastInsertRowId();
    std::cout << "[test] 插入 rowid = " << id << std::endl;

    auto rows = co_await db.Query("SELECT * FROM requests");
    std::cout << "[test] 查询到 " << rows.size() << " 行:" << std::endl;
    for (auto& row : rows) {
        for (size_t i = 0; i < row.columns.size(); ++i) {
            std::cout << "  [" << i << "] " << row.columns[i];
        }
        std::cout << std::endl;
    }

    std::cout << "[test] 全部通过" << std::endl;
    std::remove("test.db");
    std::remove("test.db-wal");
    std::remove("test.db-shm");
}

int main()
{
    try {
        asio::io_context ioc;
        asio::co_spawn(ioc, Test(ioc), asio::detached);
        asio::executor_work_guard<asio::io_context::executor_type> wg(ioc.get_executor());
        ioc.run();
    } catch (std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
