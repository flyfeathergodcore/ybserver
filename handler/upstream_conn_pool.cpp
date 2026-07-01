#include "handler/upstream_conn_pool.hpp"
#include <algorithm>
#include <iostream>

UpstreamConnPool& UpstreamConnPool::Instance()
{
    thread_local UpstreamConnPool pool;
    return pool;
}

UpstreamConnPool::~UpstreamConnPool()
{
    // Close all idle sockets before destruction
    for (auto& e : entries_) {
        if (e.conn && e.conn->socket.is_open()) {
            asio::error_code ec;
            e.conn->socket.close(ec);
        }
    }
    entries_.clear();
}

std::unique_ptr<UpstreamConnPool::Conn>
UpstreamConnPool::Acquire(const std::string& host, unsigned short port)
{
    EvictStale();

    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (it->host == host && it->port == port && it->conn)
        {
            auto conn = std::move(it->conn);
            entries_.erase(it);

            // Check if socket is still usable (quick non-intrusive check)
            if (conn->socket.is_open())
                return conn;

            // Closed by upstream — discard and continue scanning
            // (shouldn't happen since EvictStale removed closed ones,
            //  but be defensive)
        }
    }

    return nullptr;
}

void UpstreamConnPool::Release(std::unique_ptr<Conn> conn,
                                const std::string& host,
                                unsigned short port)
{
    if (!conn || !conn->socket.is_open())
        return;

    conn->last_used = std::chrono::steady_clock::now();

    // Evict one stale entry if at capacity
    if (entries_.size() >= kMaxEntries)
    {
        // Find the entry with the oldest last_used
        auto oldest = entries_.begin();
        for (auto it = entries_.begin() + 1; it != entries_.end(); ++it)
        {
            if (it->conn && it->conn->last_used < oldest->conn->last_used)
                oldest = it;
        }
        entries_.erase(oldest);
    }

    Entry entry;
    entry.conn  = std::move(conn);
    entry.host  = host;
    entry.port  = port;
    entries_.push_back(std::move(entry));
}

size_t UpstreamConnPool::IdleCount() const
{
    return entries_.size();
}

void UpstreamConnPool::EvictStale()
{
    if (entries_.empty()) return;

    auto now = std::chrono::steady_clock::now();

    for (auto it = entries_.begin(); it != entries_.end(); )
    {
        bool remove = false;
        if (!it->conn)
        {
            remove = true;
        }
        else if (!it->conn->socket.is_open())
        {
            remove = true;
        }
        else if ((now - it->conn->last_used) > kIdleTimeout)
        {
            asio::error_code ec;
            it->conn->socket.close(ec);
            remove = true;
        }

        if (remove)
            it = entries_.erase(it);
        else
            ++it;
    }
}
