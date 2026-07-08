#pragma once
#include <asio.hpp>
#include <string>
#include <vector>
#include <cstdint>

// ═══════════════════════════════════════════════════════════════════
// Database abstraction layer
//
// Pure virtual interfaces that decouple business logic from any
// specific database engine (SQLite, MySQL, PostgreSQL, etc.).
//
// To add a new backend:
//   1. Implement DbConnection (Execute / Query / LastInsertRowId)
//   2. Implement DbPool (Acquire / Release)
//   3. Register the pool in main.cpp or config
// ═══════════════════════════════════════════════════════════════════

/// Row of query results — stringly-typed for universal compatibility
/// across all database engines.
struct Row {
    std::vector<std::string> columns;
};

// ── DbConnection ──────────────────────────────────────────────────

/// Abstract database connection.
///
/// All I/O is non-blocking (coroutine-based). The concrete
/// implementation is responsible for offloading blocking work to a
/// background thread pool or using an async driver.
class DbConnection {
public:
    virtual ~DbConnection() = default;

    /// Execute SQL that does not return rows (CREATE, INSERT, UPDATE, DELETE, DDL).
    virtual asio::awaitable<void> Execute(std::string sql) = 0;

    /// Execute SQL that returns rows (SELECT, EXPLAIN, etc.).
    virtual asio::awaitable<std::vector<Row>> Query(std::string sql) = 0;

    /// Get the last inserted row ID (after an INSERT).
    virtual asio::awaitable<int64_t> LastInsertRowId() = 0;
};

// ── DbPool ────────────────────────────────────────────────────────

/// Abstract database connection pool.
///
/// Acquire / Release semantics with coroutine-friendly suspension.
/// The pool owns all connection objects; callers MUST release every
/// connection they acquire.
class DbPool {
public:
    virtual ~DbPool() = default;

    /// Acquire a connection (suspends if none available).
    virtual asio::awaitable<DbConnection*> Acquire() = 0;

    /// Return a connection to the pool.
    virtual void Release(DbConnection* conn) = 0;
};
