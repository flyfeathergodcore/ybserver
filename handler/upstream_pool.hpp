#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdint>

// ═══════════════════════════════════════════════════════════════════
// UpstreamPool — load-balanced group of backend servers
//
// Features:
//   - Round-robin selection (with per-server weights)
//   - Passive health checks: failures tracked, server auto-suspended
//     after consecutive failures; re-tried after a cooldown period.
//   - Thread-safe (atomic counter for RR, per-server state protected
//     by atomic flags so concurrent workers don't need a mutex).
// ═══════════════════════════════════════════════════════════════════

struct UpstreamServer {
    std::string host;
    unsigned short port;

    int weight = 1;       // relative weight for weighted round-robin
    int failures = 0;     // consecutive failures (reset on success)
    bool alive = true;    // suspended if failures >= threshold

    // Grace time before retrying a dead server (in milliseconds)
    static constexpr int kCooldownMs = 10'000;
    std::chrono::steady_clock::time_point dead_since;
};

class UpstreamPool {
public:
    explicit UpstreamPool(std::vector<UpstreamServer> servers);

    /// Pick a healthy backend using weighted round-robin.
    /// Returns nullptr if all servers are dead.
    const UpstreamServer* Pick();

    /// Report a failed request to the given server.
    /// Increments failure count; suspends if >= kMaxFailures.
    void ReportFailure(const UpstreamServer* server);

    /// Report a successful request — resets failure count.
    void ReportSuccess(const UpstreamServer* server);

    /// Number of servers in the pool (including dead ones).
    size_t Size() const { return servers_.size(); }

    static constexpr int kMaxFailures = 3;

private:
    /// Check if a suspended server should be retried.
    void MaybeRevive(UpstreamServer& server);

    std::vector<UpstreamServer> servers_;
    std::atomic<uint64_t> counter_{0};
};
