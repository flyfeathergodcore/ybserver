#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// ═══════════════════════════════════════════════════════════════
// Metrics — thread-local counters, lock-free ring buffer
// ═══════════════════════════════════════════════════════════════

constexpr int kLatencyBuckets  = 10;
constexpr int kRingHistory     = 60;   // 60-second sliding window
constexpr int kMaxWorkers      = 64;
constexpr int kDefaultPushMs   = 1000; // default SSE push interval

/// Bucket upper bounds in microseconds.
constexpr uint64_t kBucketMax[kLatencyBuckets] = {
    64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000, UINT64_MAX
};

// ── Per-worker hot counters (non-atomic, accessed by one thread only) ──
//
struct alignas(64) WorkerMetrics {
    /// Active connections — atomic because it's read by the HTTP handler
    /// while being written by the session loop.
    std::atomic<uint64_t> active_connections{0};

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
    int64_t           timestamp = 0;
    MetricsSnapshot   workers[kMaxWorkers];
    MetricsSnapshot   total;
};

// ── Percentile computation ──
//
struct LatencyPercentiles {
    uint64_t p50 = 0, p90 = 0, p99 = 0;
};
LatencyPercentiles ComputePercentiles(const uint64_t buckets[kLatencyBuckets]);

// ═══════════════════════════════════════════════════════════════
// Alert system
// ═══════════════════════════════════════════════════════════════

enum class AlertMetric {
    ErrorRate,    // percentage (0.0 – 100.0)
    P99Latency,   // microseconds
    QPS,          // requests/second
};

struct AlertRule {
    std::string   name;
    AlertMetric   metric;
    double        threshold;   // e.g. 0.1 for 0.1% error rate, 10000 for 10ms p99
    int           window_secs; // evaluate over last N seconds
};

/// Current state of one alert.
struct AlertState {
    std::string name;
    bool        firing = false;

    /// JSON snippet: {"name":"...","state":"firing|ok","value":...,"threshold":...}
    std::string ToJson(double current_value) const;
};

// ═══════════════════════════════════════════════════════════════
// MetricsCollector
// ═══════════════════════════════════════════════════════════════

class MetricsCollector {
public:
    explicit MetricsCollector(int num_workers);

    void SetWorkerCount(int n) { num_workers_ = n; }
    int  WorkerCount() const { return num_workers_; }

    // Alerts
    void SetAlertRules(std::vector<AlertRule> rules) { alert_rules_ = std::move(rules); }
    const std::vector<AlertState>& AlertStates() const { return alert_states_; }

    // ── Hot path (from Session) ──

    void OnRequest(uint64_t latency_us, int status_code,
                   size_t bytes, int wid);
    void OnConnectionOpen(int wid);
    void OnConnectionClose(int wid);

    // ── Per-worker flush (1-second timer) ──

    void Flush(int wid);

    // ── HTTP response builders ──

    std::string RenderMetricsJson() const;

    /// SSE: render the latest ring entry as a compact JSON line.
    /// Returns empty string if no data since @a since_ts.
    std::string RenderLatestSnapshot(int64_t since_ts) const;

    /// SSE: render fired alerts (delta since last push).
    /// Empty string if no alert state transition.
    std::string RenderAlertDelta(const std::vector<AlertState>& prev) const;

    // ── Accessors ──

    uint64_t ActiveConnections() const;
    int64_t  CurrentTimestamp() const;

    /// Timestamp of the most recently flushed ring slot.
    int64_t  LastFlushTimestamp() const;

private:
    int num_workers_;
    std::array<WorkerMetrics, kMaxWorkers> workers_{};
    std::array<RingSlot, kRingHistory> ring_{};
    std::chrono::steady_clock::time_point start_;

    // Alerts
    std::vector<AlertRule>  alert_rules_;
    std::vector<AlertState> alert_states_;

    void EvaluateAlerts(int64_t now_ts);
};
