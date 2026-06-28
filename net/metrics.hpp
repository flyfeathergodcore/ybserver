#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>

// ═══════════════════════════════════════════════════════════════
// Metrics — thread-local counters, lock-free ring buffer
// ═══════════════════════════════════════════════════════════════

constexpr int kLatencyBuckets  = 10;
constexpr int kRingHistory     = 60;   // 60-second sliding window
constexpr int kMaxWorkers      = 64;

/// Bucket upper bounds in microseconds.
/// Bucket i covers [prev, kBucketMax[i]).
constexpr uint64_t kBucketMax[kLatencyBuckets] = {
    64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000, UINT64_MAX
};

// ── Per-worker hot counters (non-atomic, accessed by one thread only) ──
//
struct alignas(64) WorkerMetrics {
    /// Active connections — atomic because it's read by the HTTP handler
    /// while being written by the session loop.
    std::atomic<uint64_t> active_connections{0};

    // Everything below is hot-path: no atomics, no false sharing with
    // adjacent WorkerMetrics objects (alignas(64) ensures cache-line isolation).
    uint64_t request_count  = 0;
    uint64_t error_count    = 0;
    uint64_t bytes_sent     = 0;
    uint64_t latency_buckets[kLatencyBuckets] = {};
};

// ── Per-second snapshot from one worker ──
//
struct MetricsSnapshot {
    uint64_t request_count  = 0;
    uint64_t error_count    = 0;
    uint64_t bytes_sent     = 0;
    uint64_t latency_buckets[kLatencyBuckets] = {};
};

// ── One ring slot: all workers' snapshots for one second ──
//
struct RingSlot {
    int64_t           timestamp = 0;  // seconds since steady_clock epoch
    MetricsSnapshot   workers[kMaxWorkers];
    MetricsSnapshot   total;          // summed across workers at flush time
};

// ═══════════════════════════════════════════════════════════════
// Percentile computation helpers
// ═══════════════════════════════════════════════════════════════

/// Compute approximate p50/p90/p99 from a latency histogram.
struct LatencyPercentiles {
    uint64_t p50 = 0, p90 = 0, p99 = 0;
};
LatencyPercentiles ComputePercentiles(const uint64_t buckets[kLatencyBuckets]);

// ═══════════════════════════════════════════════════════════════
// MetricsCollector — one per process
// ═══════════════════════════════════════════════════════════════

class MetricsCollector {
public:
    explicit MetricsCollector(int num_workers);

    void SetWorkerCount(int n) { num_workers_ = n; }
    int  WorkerCount() const { return num_workers_; }
    static int MaxWorkers() { return kMaxWorkers; }

    // ── Hot path calls (from Session) ──

    void OnRequest(uint64_t latency_us, int status_code,
                   size_t bytes, int wid);
    void OnConnectionOpen(int wid);
    void OnConnectionClose(int wid);

    // ── Per-worker flush (called by 1-second timer on each worker's ioctx) ──

    void Flush(int wid);

    // ── HTTP response builders (called from MetricsMiddleware) ──

    std::string RenderMetricsJson() const;
    static std::string_view DashboardHtml();

    // ── Accessors for the HUD (direct socket reader) ──

    uint64_t ActiveConnections() const;
    int64_t  CurrentTimestamp() const;

private:
    int num_workers_;

    // One WorkerMetrics per worker — alignas(64) guarantees no false sharing.
    std::array<WorkerMetrics, kMaxWorkers> workers_{};

    // Lock-free ring buffer: each worker independently writes to
    // `ring_[t % kRingHistory].workers[wid]` — no writer contention.
    // The `total` sub-field is best-effort (summed during Flush).
    std::array<RingSlot, kRingHistory> ring_{};

    // Start timestamp for uptime computation.
    std::chrono::steady_clock::time_point start_;
};
