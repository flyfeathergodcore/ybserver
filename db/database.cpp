#include "db/database.hpp"
#include <iostream>

Database::Database(const std::string& path, asio::thread_pool& pool)
    : pool_(pool)
{
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[db] 打开失败: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    // Enable WAL mode so reads don't block writes and vice versa
    char* err = nullptr;
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err);
    if (err) {
        std::cerr << "[db] WAL 设置失败: " << err << std::endl;
        sqlite3_free(err);
    }
    std::cout << "[db] 已打开: " << path << std::endl;
}

Database::~Database()
{
    if (db_) {
        sqlite3_close(db_);
    }
}

bool Database::IsOpen() const { return db_ != nullptr; }

asio::awaitable<void> Database::Execute(std::string sql)
{
    auto exec = co_await asio::this_coro::executor;

    if (!db_) {
        std::cerr << "[db] 数据库未打开，拒绝执行: " << sql << std::endl;
        co_return;
    }

    // Switch to thread_pool for blocking SQLite call
    co_await asio::post(pool_, asio::use_awaitable);

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[db] Execute 失败: " << err << std::endl;
        sqlite3_free(err);
    }

    co_await asio::post(exec, asio::use_awaitable);
}

asio::awaitable<std::vector<Row>> Database::Query(std::string sql)
{
    auto exec = co_await asio::this_coro::executor;
    std::vector<Row> result;

    if (!db_) {
        std::cerr << "[db] 数据库未打开，拒绝查询: " << sql << std::endl;
        co_return result;
    }

    co_await asio::post(pool_, asio::use_awaitable);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[db] 预编译失败: " << sqlite3_errmsg(db_) << std::endl;
        co_await asio::post(exec, asio::use_awaitable);
        co_return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        int col_count = sqlite3_column_count(stmt);
        for (int i = 0; i < col_count; ++i) {
            auto text = sqlite3_column_text(stmt, i);
            row.columns.push_back(text
                ? reinterpret_cast<const char*>(text)
                : "");
        }
        result.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);

    co_await asio::post(exec, asio::use_awaitable);
    co_return result;
}

asio::awaitable<int64_t> Database::LastInsertRowId()
{
    auto exec = co_await asio::this_coro::executor;

    if (!db_) {
        co_return 0;
    }

    co_await asio::post(pool_, asio::use_awaitable);
    int64_t id = static_cast<int64_t>(sqlite3_last_insert_rowid(db_));
    co_await asio::post(exec, asio::use_awaitable);
    co_return id;
}
