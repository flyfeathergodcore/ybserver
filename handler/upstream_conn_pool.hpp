#pragma once
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <asio.hpp>

// ═══════════════════════════════════════════════════════════════════
// UpstreamConnPool — per-worker TCP connection reuse for proxy
//
// Each worker thread has its own thread_local pool.  Connections are
// tagged with (host, port) so Acquire() returns an idle socket to the
// right upstream.
//
// Idle connections are evicted after kIdleTimeout (30 s) on the next
// Acquire/Release.  The pool is capped at kMaxEntries per worker.
//
// Thread safety: thread_local — no locking needed.
// ═══════════════════════════════════════════════════════════════════

class UpstreamConnPool {
public:
    /// A pooled TCP connection.
    struct Conn {
        asio::ip::tcp::socket socket;
        std::chrono::steady_clock::time_point last_used;

        explicit Conn(asio::any_io_executor exec)
            : socket(std::move(exec)) {}
    };

    /// Thread-local singleton.
    static UpstreamConnPool& Instance();

    /// Acquire an idle connection for (host, port).
    /// Returns nullptr if no matching connection is available.
    std::unique_ptr<Conn> Acquire(const std::string& host,
                                  unsigned short port);

    /// Return a connection to the pool for reuse.
    /// The pool takes ownership — do not use `conn` after calling.
    void Release(std::unique_ptr<Conn> conn,
                 const std::string& host,
                 unsigned short port);

    /// Number of idle connections (for metrics / debugging).
    size_t IdleCount() const;

private:
    UpstreamConnPool() = default;
    ~UpstreamConnPool();

    UpstreamConnPool(const UpstreamConnPool&) = delete;
    UpstreamConnPool& operator=(const UpstreamConnPool&) = delete;

    void EvictStale();

    struct Entry {
        std::unique_ptr<Conn> conn;
        std::string host;
        unsigned short port;
    };

    std::vector<Entry> entries_;

    static constexpr auto kIdleTimeout = std::chrono::seconds(30);
    static constexpr size_t kMaxEntries = 64;
};
