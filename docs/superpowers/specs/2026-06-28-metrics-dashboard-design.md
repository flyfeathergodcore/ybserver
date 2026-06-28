# Metrics & Dashboard Design

## Architecture

```
Worker 0 (std::jthread)             Worker 1 (std::jthread)
┌──────────────────────────┐       ┌──────────────────────────┐
│ WorkerMetrics[0]         │       │ WorkerMetrics[1]         │
│  active_connections ▸▲   │       │  active_connections ▸▲   │
│  thread:                 │       │  thread:                 │
│   request_count++        │       │   request_count++        │
│   latency_buckets[bin]++ │       │   latency_buckets[bin]++ │
│   bytes_sent += N        │       │   bytes_sent += N        │
│   error_count++          │       │   error_count++          │
│                          │       │                          │
│ Flush every 1s:          │       │ Flush every 1s:          │
│  → ring_[pos].workers[0] │       │  → ring_[pos].workers[1] │
└──────────────────────────┘       └──────────────────────────┘
         │                                   │
         └───────────────┬───────────────────┘
                         ▼
         ┌──────────────────────────────────┐
         │  MetricsCollector                │
         │  ring_[60] SlotSnapshot × 60s    │
         │  ▲ each slot has kMaxWorkers     │
         │  │  per-worker ThreadMetrics     │
         └──────────────────────────────────┘
                         │
           ┌─────────────┴─────────────┐
           ▼                           ▼
  /metrics.json (REST)         /dashboard (HTML+JS)
  GET → JSON stats             GET → full page, JS polls
  (polled by HUD/curl)         every 1s via setInterval
```

## Collection

- **hot path** (every request): write to `thread_local`-style WorkerMetrics slot (owned by worker thread, no atomics on hot counters)
- **active_connections**: `std::atomic<uint64_t>` per worker, written once per connection open/close, read by HTTP handler
- **every 1s**: per-worker `asio::steady_timer` calls `Flush(wid)` — memcpy of thread counters into shared ring buffer at current ring position

## Latency Buckets (10 bins)

```
0:    <64µs    1:   <128µs    2:   <256µs    3:   <512µs
4:    <1ms     5:   <2ms     6:    <4ms     7:    <8ms
8:   <16ms    9:   ≥16ms
```

Percentiles (p50/p90/p99) computed from histogram via linear interpolation within the target bucket.

## HTTP Endpoints

Both served from the **same HTTPS server** via a middleware that intercepts these paths before reaching StaticFileHandler:

| Path | Method | Response |
|------|--------|---------|
| `/metrics.json` | GET | `application/json` — current + 60s history |
| `/dashboard` | GET | `text/html` — full dashboard page |

### Dashboard HTML Page

- Chart.js from CDN
- Three chart panels: QPS & Error Rate / Latency Percentiles / Active Connections
- 1-second polling via `setInterval(fetch("/metrics.json"), 1000)`
- Dark theme, full-width layout
- 60-second sliding window

### metrics.json Format

```json
{
  "active_connections": 42,
  "history": [
    {"t": 1712345678, "qps": 72000, "errors": 0, "bytes": 7123456,
     "p50": 1160, "p90": 2790, "p99": 5260},
    ...
  ]
}
```

## Terminal HUD

Standalone C++ program (uses asio, no OpenSSL needed):

- Connects via **plain HTTP** to a separate `metrics_port` (configurable, default 9090, bind 127.0.0.1)
- Polls `/metrics.json` every second
- Renders with ANSI escape codes (colored bars for QPS, p50/p90/p99, error rate)
- Vertical layout: QPS gauge bar, latency row, error rate, active connections
- `Ctrl+C` to exit, terminal restored on exit

## Integration Points

1. **Worker struct** — add `int metrics_id`, `MetricsCollector*`, flush timer
2. **Session** — add `MetricsCollector*`, `int worker_id_`
3. **Session::Start()** — call `OnConnectionOpen`, `OnRequest(latency, status, bytes)`, `OnConnectionClose`
4. **Middleware chain** — add `MetricsMiddleware` at front, intercepts `/metrics.json` and `/dashboard/`
5. **main.cpp** — add `MetricsMiddleware` to chain before logging middleware

## Files

| File | Action |
|------|--------|
| `net/metrics.hpp` | Create — MetricsCollector, WorkerMetrics, ThreadMetrics |
| `net/metrics.cpp` | Create — collection, JSON rendering, dashboard HTML |
| `middleware/metrics_middleware.hpp` | Create — MetricsMiddleware class |
| `tools/hud.cpp` | Create — terminal HUD |
| `net/multi_server.hpp` | Modify — add MetricsCollector, flush timer to Worker |
| `net/multi_server.cpp` | Modify — create MetricsCollector, wire flush timers |
| `net/session.hpp` | Modify — add MetricsCollector pointer, worker_id |
| `net/session.cpp` | Modify — metrics calls in Start() + Send() |
| `middleware/middleware.hpp` | Modify — forward declare MetricsMiddleware |
| `main.cpp` | Modify — add MetricsMiddleware |
| `config.yaml` | Modify — optional metrics_port |
| `config/config.hpp` | Modify — add metrics_port field |
| `CMakeLists.txt` | Modify — add metrics.cpp, hud |
