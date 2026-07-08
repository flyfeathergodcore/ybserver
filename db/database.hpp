#pragma once
#include "db/db_interface.hpp"
#include <asio.hpp>
#include <sqlite3.h>
#include <string>
#include <vector>

// Async SQLite wrapper — implements DbConnection.
//
// All database operations are offloaded to a thread_pool to avoid
// blocking the io_context event loop.
//
// Usage:
//   asio::thread_pool db_pool(2);
//   Database db("server.db", db_pool);
//   co_await db.Execute("CREATE TABLE IF NOT EXISTS ...");
//   auto rows = co_await db.Query("SELECT * FROM t");
class Database : public DbConnection {
public:
    Database(const std::string& path, asio::thread_pool& pool);
    ~Database() override;

    // ── DbConnection ──
    asio::awaitable<void> Execute(std::string sql) override;
    asio::awaitable<std::vector<Row>> Query(std::string sql) override;
    asio::awaitable<int64_t> LastInsertRowId() override;

private:
    sqlite3* db_ = nullptr;
    asio::thread_pool& pool_;
};
