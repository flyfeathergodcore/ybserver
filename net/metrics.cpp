#include "net/metrics.hpp"
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

    // Compute thresholds between buckets
    // Bucket 0: [0, 64), Bucket 1: [64, 128), ... Bucket 9: [16000, ∞)
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
            // Linear interpolation within the bucket
            double frac = 0.0;
            if (cum != prev_cum)
                frac = (threshold - prev_cum) / (cum - prev_cum);
            *results[ti] = lower_bound[i]
                + static_cast<uint64_t>(frac * (kBucketMax[i] - lower_bound[i]));
            ti++;
            if (ti < 3)
                threshold = static_cast<double>(targets[ti]) / 100.0 * total;
        }
    }

    // Clamp remaining percentiles to max
    for (; ti < 3; ti++)
        *results[ti] = 16000;

    return p;
}

// ═══════════════════════════════════════════════════════════════
// MetricsCollector
// ═══════════════════════════════════════════════════════════════

MetricsCollector::MetricsCollector(int num_workers)
    : num_workers_(num_workers)
    , start_(std::chrono::steady_clock::now())
{
}

void MetricsCollector::OnRequest(uint64_t latency_us, int status_code,
                                  size_t bytes, int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];
    w.request_count++;

    if (status_code < 200 || status_code >= 300)
        w.error_count++;

    w.bytes_sent += bytes;

    // Assign to latency bucket
    int bucket = kLatencyBuckets - 1;  // default: last bucket (≥16ms)
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
    workers_[wid].active_connections.fetch_add(1,
        std::memory_order_relaxed);
}

void MetricsCollector::OnConnectionClose(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    workers_[wid].active_connections.fetch_sub(1,
        std::memory_order_relaxed);
}

void MetricsCollector::Flush(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];

    // Snapshot hot counters
    MetricsSnapshot snap;
    snap.request_count = w.request_count;
    snap.error_count   = w.error_count;
    snap.bytes_sent    = w.bytes_sent;
    std::memcpy(snap.latency_buckets, w.latency_buckets,
                sizeof(snap.latency_buckets));

    // Reset hot counters
    w.request_count = 0;
    w.error_count   = 0;
    w.bytes_sent    = 0;
    std::memset(w.latency_buckets, 0, sizeof(w.latency_buckets));

    // Write to ring slot
    auto now = std::chrono::steady_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    int slot = static_cast<int>(secs % kRingHistory);

    ring_[slot].timestamp = secs;
    ring_[slot].workers[wid] = snap;

    // Recompute total for this slot (sum across all workers)
    MetricsSnapshot total{};
    for (int i = 0; i < kMaxWorkers; i++)
    {
        auto& ws = ring_[slot].workers[i];
        total.request_count += ws.request_count;
        total.error_count   += ws.error_count;
        total.bytes_sent    += ws.bytes_sent;
        for (int b = 0; b < kLatencyBuckets; b++)
            total.latency_buckets[b] += ws.latency_buckets[b];
    }
    ring_[slot].total = total;
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
// JSON rendering
// ═══════════════════════════════════════════════════════════════

std::string MetricsCollector::RenderMetricsJson() const
{
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        now - start_).count();

    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    int current_slot = static_cast<int>(now_secs % kRingHistory);

    // Build JSON
    std::string json;
    json.reserve(8192);

    // Header
    json += "{\n";
    json += "  \"active_connections\": ";
    json += std::to_string(ActiveConnections());
    json += ",\n";
    json += "  \"uptime_seconds\": ";
    json += std::to_string(uptime);
    json += ",\n";
    json += "  \"history\": [\n";

    // Walk ring buffer (oldest first)
    int count = 0;
    for (int i = 0; i < kRingHistory; i++)
    {
        int idx = (current_slot + 1 + i) % kRingHistory;
        auto& slot = ring_[idx];
        if (slot.timestamp == 0) continue;
        auto& t = slot.total;
        if (t.request_count == 0) continue;

        auto per = ComputePercentiles(t.latency_buckets);

        if (count > 0) json += ",\n";

        json += "    {\"t\":";
        json += std::to_string(slot.timestamp);
        json += ",\"qps\":";
        json += std::to_string(t.request_count);
        json += ",\"err\":";
        json += std::to_string(t.error_count);
        json += ",\"bytes\":";
        json += std::to_string(t.bytes_sent);
        json += ",\"p50\":";
        json += std::to_string(per.p50);
        json += ",\"p90\":";
        json += std::to_string(per.p90);
        json += ",\"p99\":";
        json += std::to_string(per.p99);
        json += ",\"act\":";
        json += std::to_string(ActiveConnections());
        json += "}";
        count++;
    }

    json += "\n  ]\n}\n";
    return json;
}

// ═══════════════════════════════════════════════════════════════
// Dashboard HTML (embedded constant)
// ═══════════════════════════════════════════════════════════════

std::string_view MetricsCollector::DashboardHtml()
{
    // Inline HTML with Chart.js from CDN.
    // Dark theme, three panels: QPS+Errors / Latency percentiles / Active connections.
    // Polls /metrics.json every second via setInterval.
    return R"HTML(<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HTTP Server Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,-apple-system,sans-serif;padding:24px}
h1{font-size:20px;margin-bottom:16px;color:#e94560}
.chart-grid{display:grid;grid-template-columns:1fr;gap:16px;max-width:1200px}
.chart-box{background:#16213e;border-radius:8px;padding:16px}
.chart-box h2{font-size:14px;color:#aaa;margin-bottom:8px}
.summary{display:flex;gap:24px;margin-bottom:16px;flex-wrap:wrap}
.stat{background:#16213e;border-radius:8px;padding:12px 20px;min-width:100px}
.stat-label{font-size:11px;color:#888;text-transform:uppercase}
.stat-value{font-size:24px;font-weight:600;color:#e94560;margin-top:2px}
.stat-value.up{color:#53d769}
.stat-value.warn{color:#ffa726}
</style>
</head>
<body>
<h1>⚡ HTTP Server Dashboard</h1>
<div class="summary">
  <div class="stat"><div class="stat-label">Requests/s</div><div class="stat-value" id="cur-qps">—</div></div>
  <div class="stat"><div class="stat-label">Errors/s</div><div class="stat-value" id="cur-err">—</div></div>
  <div class="stat"><div class="stat-label">p50 / p90 / p99</div><div class="stat-value" id="cur-lat">—</div></div>
  <div class="stat"><div class="stat-label">Active</div><div class="stat-value" id="cur-act">—</div></div>
  <div class="stat"><div class="stat-label">Uptime</div><div class="stat-value" id="cur-uptime">—</div></div>
</div>
<div class="chart-grid">
  <div class="chart-box"><h2>QPS & Errors</h2><canvas id="chart-qps" height="200"></canvas></div>
  <div class="chart-box"><h2>Latency (p50 / p90 / p99)</h2><canvas id="chart-latency" height="200"></canvas></div>
  <div class="chart-box"><h2>Active Connections</h2><canvas id="chart-connections" height="160"></canvas></div>
</div>
<script>
const labels=[],qpsData=[],errData=[],p50Data=[],p90Data=[],p99Data=[],actData=[];
const MAX=60;
function upd(){fetch('/metrics.json').then(r=>r.json()).then(d=>{
  document.getElementById('cur-act').textContent=d.active_connections;
  document.getElementById('cur-uptime').textContent=(d.uptime_seconds/60).toFixed(1)+'m';
  const h=d.history;
  if(!h||!h.length)return;
  const last=h[h.length-1];
  document.getElementById('cur-qps').textContent=last.qps;
  document.getElementById('cur-err').textContent=last.err;
  document.getElementById('cur-lat').textContent=last.p50+'/'+last.p90+'/'+last.p99+'µs';
  labels.length=0;qpsData.length=0;errData.length=0;
  p50Data.length=0;p90Data.length=0;p99Data.length=0;actData.length=0;
  const start=Math.max(0,h.length-MAX);
  for(let i=start;i<h.length;i++){
    labels.push((i-start)+'s');
    qpsData.push(h[i].qps);errData.push(h[i].err);
    p50Data.push(h[i].p50);p90Data.push(h[i].p90);p99Data.push(h[i].p99);
    actData.push(h[i].act||d.active_connections);
  }
  QPS.update();LAT.update();ACT.update();
}).catch(()=>{});
}
const common={responsive:true,maintainAspectRatio:false,
  scales:{x:{display:false},y:{beginAtZero:true,grid:{color:'#2a2a4e'}}},
  plugins:{legend:{labels:{color:'#aaa',boxWidth:12,font:{size:11}}}}};

const QPS=new Chart(document.getElementById('chart-qps'),{type:'line',data:{
  labels,datasets:[
    {label:'QPS',data:qpsData,borderColor:'#53d769',backgroundColor:'rgba(83,215,105,0.1)',fill:true,pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'Errors',data:errData,borderColor:'#e94560',backgroundColor:'rgba(233,69,96,0.1)',fill:true,pointRadius:0,borderWidth:1.5,tension:.3}
  ]},options:{...common,plugins:{...common.plugins,legend:{...common.plugins.legend,labels:{...common.plugins.legend.labels}}}}});

const LAT=new Chart(document.getElementById('chart-latency'),{type:'line',data:{
  labels,datasets:[
    {label:'p50',data:p50Data,borderColor:'#53d769',pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'p90',data:p90Data,borderColor:'#ffa726',pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'p99',data:p99Data,borderColor:'#e94560',pointRadius:0,borderWidth:1.5,tension:.3}
  ]},options:{...common,plugins:{...common.plugins,legend:{...common.plugins.legend,labels:{...common.plugins.legend.labels}}}}});

const ACT=new Chart(document.getElementById('chart-connections'),{type:'line',data:{
  labels,datasets:[
    {label:'Active',data:actData,borderColor:'#42a5f5',backgroundColor:'rgba(66,165,245,0.15)',fill:true,pointRadius:0,borderWidth:2,tension:.3}
  ]},options:{...common,plugins:{...common.plugins,legend:{...common.plugins.legend,labels:{...common.plugins.legend.labels}}}}});

upd();setInterval(upd,1000);
</script>
</body>
</html>)HTML";
}
