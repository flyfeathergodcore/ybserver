#pragma once
#include <asio.hpp>
#include <sqlite3.h>
#include <string>
#include <vector>

struct Row {
    std::vector<std::string> columns;
};

// Async SQLite wrapper — runs all database operations on a thread_pool
// to avoid blocking the io_context event loop.
//
// Usage:
//   asio::thread_pool db_pool(2);
//   Database db("server.db", db_pool);
//   co_await db.Execute("CREATE TABLE IF NOT EXISTS ...");
//   auto rows = co_await db.Query("SELECT * FROM t");
class Database {
public:
    Database(const std::string& path, asio::thread_pool& pool);
    ~Database();

    // Execute SQL that does not return rows (CREATE, INSERT, UPDATE, DELETE)
    asio::awaitable<void> Execute(std::string sql);

    // Execute SQL that returns rows (SELECT)
    asio::awaitable<std::vector<Row>> Query(std::string sql);

    // Get the last inserted rowid
    asio::awaitable<sqlite3_int64> LastInsertRowId();

private:
    sqlite3* db_ = nullptr;
    asio::thread_pool& pool_;
};
