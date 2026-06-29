#include "handler/metrics.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cassert>

// ═══════════════════════════════════════════════════════════════
// Percentile computation
// ═══════════════════════════════════════════════════════════════

LatencyPercentiles ComputePercentiles(const uint64_t buckets[kLatencyBuckets])
{
    LatencyPercentiles p{};

    constexpr uint64_t lower_bound[kLatencyBuckets] = {
        0, 64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000
    };

    uint64_t total = 0;
    for (int i = 0; i < kLatencyBuckets; i++)
        total += buckets[i];

    if (total == 0) return p;

    constexpr uint64_t targets[3] = {50, 90, 99};
    uint64_t* results[3] = {&p.p50, &p.p90, &p.p99};
    int ti = 0;

    uint64_t cum = 0;
    for (int i = 0; i < kLatencyBuckets && ti < 3; i++)
    {
        if (buckets[i] == 0) continue;
        uint64_t prev_cum = cum;
        cum += buckets[i];
        double threshold = static_cast<double>(targets[ti]) / 100.0 * total;

        while (ti < 3 && static_cast<double>(cum) >= threshold)
        {
            double frac = 0.0;
            if (cum != prev_cum)
                frac = (threshold - prev_cum) / (cum - prev_cum);
            // For the last bucket (≥16ms), UINT64_MAX as upper bound causes
            // garbage in double arithmetic. Use a fixed 16ms extension.
            uint64_t range = (i == kLatencyBuckets - 1)
                ? kBucketMax[kLatencyBuckets - 2]  // 16000
                : (kBucketMax[i] - lower_bound[i]);
            *results[ti] = lower_bound[i]
                + static_cast<uint64_t>(frac * static_cast<double>(range));
            ti++;
            if (ti < 3)
                threshold = static_cast<double>(targets[ti]) / 100.0 * total;
        }
    }

    for (; ti < 3; ti++)
        *results[ti] = 16000;

    return p;
}

// ═══════════════════════════════════════════════════════════════
// AlertState
// ═══════════════════════════════════════════════════════════════

std::string AlertState::ToJson(double current_value) const
{
    std::string j = "{\"name\":\"";
    j += name;
    j += "\",\"state\":\"";
    j += firing ? "firing" : "ok";
    j += "\",\"value\":";
    j += std::to_string(current_value);
    j += "}";
    return j;
}

// ═══════════════════════════════════════════════════════════════
// MetricsCollector
// ═══════════════════════════════════════════════════════════════

MetricsCollector::MetricsCollector(int num_workers)
    : num_workers_(num_workers)
    , start_(std::chrono::steady_clock::now())
{
    // Default alert rules
    alert_rules_ = {
        {"high-error-rate", AlertMetric::ErrorRate, 0.1, 5},   // >0.1% errors
        {"high-p99",       AlertMetric::P99Latency, 10000, 5},  // >10ms
        {"qps-plummet",    AlertMetric::QPS,        1000,   10}, // <1000 qps
    };
    alert_states_.resize(alert_rules_.size());
    for (size_t i = 0; i < alert_rules_.size(); i++)
        alert_states_[i].name = alert_rules_[i].name;
}

void MetricsCollector::OnRequest(uint64_t latency_us, int status_code,
                                  size_t bytes, int wid, bool is_h2)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];
    w.request_count++;
    if (is_h2) w.request_h2++;
    else       w.request_h1++;

    if (status_code < 200 || status_code >= 300) {
        w.error_count++;
        if (is_h2) w.error_h2++;
        else       w.error_h1++;
    }

    w.bytes_sent += bytes;

    int bucket = kLatencyBuckets - 1;
    for (int i = 0; i < kLatencyBuckets - 1; i++)
    {
        if (latency_us < kBucketMax[i]) {
            bucket = i;
            break;
        }
    }
    w.latency_buckets[bucket]++;
}

void MetricsCollector::OnConnectionOpen(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    workers_[wid].active_connections.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::OnConnectionClose(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    workers_[wid].active_connections.fetch_sub(1, std::memory_order_relaxed);
}

void MetricsCollector::Flush(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];

    MetricsSnapshot snap;
    snap.request_count = w.request_count;
    snap.request_h1    = w.request_h1;
    snap.request_h2    = w.request_h2;
    snap.error_count   = w.error_count;
    snap.error_h1      = w.error_h1;
    snap.error_h2      = w.error_h2;
    snap.bytes_sent    = w.bytes_sent;
    std::memcpy(snap.latency_buckets, w.latency_buckets,
                sizeof(snap.latency_buckets));

    w.request_count = 0;
    w.request_h1    = 0;
    w.request_h2    = 0;
    w.error_count   = 0;
    w.error_h1      = 0;
    w.error_h2      = 0;
    w.bytes_sent    = 0;
    std::memset(w.latency_buckets, 0, sizeof(w.latency_buckets));

    auto now = std::chrono::steady_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    int slot = static_cast<int>(secs % kRingHistory);

    ring_[slot].timestamp = secs;
    ring_[slot].workers[wid] = snap;

    MetricsSnapshot total{};
    for (int i = 0; i < kMaxWorkers; i++)
    {
        auto& ws = ring_[slot].workers[i];
        total.request_count += ws.request_count;
        total.request_h1    += ws.request_h1;
        total.request_h2    += ws.request_h2;
        total.error_count   += ws.error_count;
        total.error_h1      += ws.error_h1;
        total.error_h2      += ws.error_h2;
        total.bytes_sent    += ws.bytes_sent;
        for (int b = 0; b < kLatencyBuckets; b++)
            total.latency_buckets[b] += ws.latency_buckets[b];
    }
    ring_[slot].total = total;

    // Evaluate alerts on worker 0's flush only (avoid duplicate evaluation)
    if (wid == 0)
        EvaluateAlerts(secs);
}

void MetricsCollector::EvaluateAlerts(int64_t now_ts)
{
    // Gather per-second totals over the window from the ring buffer
    for (size_t i = 0; i < alert_rules_.size(); i++)
    {
        auto& rule = alert_rules_[i];
        auto& state = alert_states_[i];

        // Sum over the window
        uint64_t sum_requests = 0;
        uint64_t sum_errors   = 0;
        uint64_t sum_p99_buckets[kLatencyBuckets] = {};

        for (int s = 0; s < std::min(rule.window_secs, kRingHistory); s++)
        {
            int idx = static_cast<int>((now_ts - s) % kRingHistory);
            auto& slot = ring_[idx];
            if (slot.timestamp == 0) continue;
            // Only include slots within window_secs of now
            if (now_ts - slot.timestamp > rule.window_secs) continue;

            sum_requests += slot.total.request_count;
            sum_errors   += slot.total.error_count;
            for (int b = 0; b < kLatencyBuckets; b++)
                sum_p99_buckets[b] += slot.total.latency_buckets[b];
        }

        double current_value = 0.0;
        bool breached = false;

        switch (rule.metric)
        {
        case AlertMetric::ErrorRate:
            current_value = (sum_requests > 0)
                ? 100.0 * static_cast<double>(sum_errors) / sum_requests
                : 0.0;
            breached = (current_value > rule.threshold);
            break;

        case AlertMetric::P99Latency: {
            auto per = ComputePercentiles(sum_p99_buckets);
            current_value = static_cast<double>(per.p99);
            breached = (current_value > rule.threshold);
            break;
        }

        case AlertMetric::QPS: {
            double avg_qps = (rule.window_secs > 0)
                ? static_cast<double>(sum_requests) / rule.window_secs
                : 0.0;
            current_value = avg_qps;
            // threshold is the MINIMUM, breach when below
            breached = (rule.threshold > 0 && current_value < rule.threshold);
            break;
        }
        }

        state.firing = breached;
        // current_value is stored for the delta renderer
        // but we don't keep it per-alert; it's computed on-demand.
        (void)current_value;
    }
}

uint64_t MetricsCollector::ActiveConnections() const
{
    uint64_t total = 0;
    for (int i = 0; i < kMaxWorkers; i++)
        total += workers_[i].active_connections.load(std::memory_order_relaxed);
    return total;
}

int64_t MetricsCollector::CurrentTimestamp() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ═══════════════════════════════════════════════════════════════
// JSON rendering (full history)
// ═══════════════════════════════════════════════════════════════

static void AppendJsonEntry(std::string& json, int64_t ts,
                             uint64_t qps, uint64_t err, uint64_t bytes,
                             const LatencyPercentiles& per,
                             uint64_t act,
                             bool first,
                             uint64_t qps_h1 = 0, uint64_t qps_h2 = 0,
                             uint64_t err_h1 = 0, uint64_t err_h2 = 0)
{
    if (!first) json += ",\n";
    json += "    {\"t\":";
    json += std::to_string(ts);
    json += ",\"qps\":";
    json += std::to_string(qps);
    json += ",\"qps_h1\":";
    json += std::to_string(qps_h1);
    json += ",\"qps_h2\":";
    json += std::to_string(qps_h2);
    json += ",\"err\":";
    json += std::to_string(err);
    json += ",\"err_h1\":";
    json += std::to_string(err_h1);
    json += ",\"err_h2\":";
    json += std::to_string(err_h2);
    json += ",\"bytes\":";
    json += std::to_string(bytes);
    json += ",\"p50\":";
    json += std::to_string(per.p50);
    json += ",\"p90\":";
    json += std::to_string(per.p90);
    json += ",\"p99\":";
    json += std::to_string(per.p99);
    json += ",\"act\":";
    json += std::to_string(act);
    json += "}";
}

std::string MetricsCollector::RenderMetricsJson() const
{
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        now - start_).count();

    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    int current_slot = static_cast<int>(now_secs % kRingHistory);

    std::string json;
    json.reserve(8192);

    json += "{\n";
    json += "  \"active_connections\": ";
    json += std::to_string(ActiveConnections());
    json += ",\n";
    json += "  \"uptime_seconds\": ";
    json += std::to_string(uptime);
    json += ",\n";
    json += "  \"alerts\": [\n";

    // Alert states
    for (size_t i = 0; i < alert_states_.size(); i++)
    {
        if (i > 0) json += ",\n";
        json += "    ";
        json += alert_states_[i].ToJson(0.0);
    }
    json += "\n  ],\n";

    json += "  \"history\": [\n";

    int count = 0;
    for (int i = 0; i < kRingHistory; i++)
    {
        int idx = (current_slot + 1 + i) % kRingHistory;
        auto& slot = ring_[idx];
        if (slot.timestamp == 0) continue;
        if (slot.total.request_count == 0 && slot.total.error_count == 0) continue;

        auto per = ComputePercentiles(slot.total.latency_buckets);
        AppendJsonEntry(json, slot.timestamp,
                         slot.total.request_count,
                         slot.total.error_count,
                         slot.total.bytes_sent,
                         per, ActiveConnections(),
                         count == 0,
                         slot.total.request_h1,
                         slot.total.request_h2,
                         slot.total.error_h1,
                         slot.total.error_h2);
        count++;
    }

    json += "\n  ]\n}\n";
    return json;
}

// ═══════════════════════════════════════════════════════════════
// SSE delta rendering (single latest entry)
// ═══════════════════════════════════════════════════════════════

int64_t MetricsCollector::LastFlushTimestamp() const
{
    int64_t best = 0;
    for (int i = 0; i < kRingHistory; i++) {
        auto ts = ring_[i].timestamp;
        if (ts > best) best = ts;
    }
    return best;
}

std::string MetricsCollector::RenderLatestSnapshot(int64_t since_ts) const
{
    // Find the most recently flushed slot (highest timestamp),
    // rather than indexing by current_time % kRingHistory, which
    // can miss flushes that land on a different slot.
    int best_slot = -1;
    int64_t best_ts = -1;
    for (int i = 0; i < kRingHistory; i++) {
        auto ts = ring_[i].timestamp;
        if (ts > best_ts) {
            best_ts = ts;
            best_slot = i;
        }
    }

    if (best_slot < 0 || best_ts <= since_ts) return {};

    auto& s = ring_[best_slot];
    auto per = ComputePercentiles(s.total.latency_buckets);
    uint64_t act = ActiveConnections();

    std::string j;
    j += "{\"t\":";
    j += std::to_string(s.timestamp);
    j += ",\"qps\":";
    j += std::to_string(s.total.request_count);
    j += ",\"qps_h1\":";
    j += std::to_string(s.total.request_h1);
    j += ",\"qps_h2\":";
    j += std::to_string(s.total.request_h2);
    j += ",\"err\":";
    j += std::to_string(s.total.error_count);
    j += ",\"err_h1\":";
    j += std::to_string(s.total.error_h1);
    j += ",\"err_h2\":";
    j += std::to_string(s.total.error_h2);
    j += ",\"bytes\":";
    j += std::to_string(s.total.bytes_sent);
    j += ",\"p50\":";
    j += std::to_string(per.p50);
    j += ",\"p90\":";
    j += std::to_string(per.p90);
    j += ",\"p99\":";
    j += std::to_string(per.p99);
    j += ",\"act\":";
    j += std::to_string(act);
    j += "}";
    return j;
}

// ═══════════════════════════════════════════════════════════════
// SSE: fired alerts delta
// ═══════════════════════════════════════════════════════════════

std::string MetricsCollector::RenderAlertDelta(
    const std::vector<AlertState>& prev) const
{
    std::string out;
    for (size_t i = 0; i < alert_states_.size(); i++)
    {
        bool changed = (i >= prev.size())
                     || (alert_states_[i].firing != prev[i].firing);
        if (!changed) continue;

        if (!out.empty()) out += "\n";
        out += "event: alert\ndata: ";
        out += alert_states_[i].ToJson(0.0);
        out += "\n";
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════
// Dashboard HTML (embedded, uses EventSource for SSE)
// ═══════════════════════════════════════════════════════════════
// NOTE: Dashboard HTML/CSS/JS are now static files at
//   www/dashboard/index.html   www/dashboard/style.css   www/dashboard/app.js
//   Served by StaticFileHandler. /metrics.json and /metrics/stream
//   remain handled by MetricsMiddleware.
// ═══════════════════════════════════════════════════════════════