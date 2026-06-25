// ─────────────────────────────────────────────────────────────
// Phase 3 — 协程版 SQLite 数据库封装
//
// 使用 asio::thread_pool 执行同步 SQLite 查询，
// 避免阻塞 io_context 事件循环。
//
// 用法：
//   asio::thread_pool db_pool(2);
//   Database db("server.db", db_pool);
//   co_await db.Execute("CREATE TABLE IF NOT EXISTS ...");
//   auto rows = co_await db.Query("SELECT * FROM requests");
// ─────────────────────────────────────────────────────────────

#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <asio.hpp>
#include <sqlite3.h>
#include <string>
#include <vector>
#include <functional>

struct Row {
    std::vector<std::string> columns;
};

class Database {
public:
    Database(const std::string& path, asio::thread_pool& pool);
    ~Database();

    // 不返回行的 SQL（CREATE, INSERT, UPDATE, DELETE）
    asio::awaitable<void> Execute(std::string sql);

    // 返回行的 SQL（SELECT）
    asio::awaitable<std::vector<Row>> Query(std::string sql);

    // 获取上次插入的 rowid
    asio::awaitable<sqlite3_int64> LastInsertRowId();

private:
    sqlite3* db_ = nullptr;
    asio::thread_pool& pool_;
};

#endif
