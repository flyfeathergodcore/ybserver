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
            *results[ti] = lower_bound[i]
                + static_cast<uint64_t>(frac * (kBucketMax[i] - lower_bound[i]));
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
                                  size_t bytes, int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];
    w.request_count++;

    if (status_code < 200 || status_code >= 300)
        w.error_count++;

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
    snap.error_count   = w.error_count;
    snap.bytes_sent    = w.bytes_sent;
    std::memcpy(snap.latency_buckets, w.latency_buckets,
                sizeof(snap.latency_buckets));

    w.request_count = 0;
    w.error_count   = 0;
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
        total.error_count   += ws.error_count;
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
                             bool first)
{
    if (!first) json += ",\n";
    json += "    {\"t\":";
    json += std::to_string(ts);
    json += ",\"qps\":";
    json += std::to_string(qps);
    json += ",\"err\":";
    json += std::to_string(err);
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
                         count == 0);
        count++;
    }

    json += "\n  ]\n}\n";
    return json;
}

// ═══════════════════════════════════════════════════════════════
// SSE delta rendering (single latest entry)
// ═══════════════════════════════════════════════════════════════

std::string MetricsCollector::RenderLatestSnapshot(int64_t since_ts) const
{
    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int slot = static_cast<int>(now_secs % kRingHistory);
    auto& s = ring_[slot];

    // No new data yet
    if (s.timestamp <= since_ts || s.timestamp == 0) return {};

    auto per = ComputePercentiles(s.total.latency_buckets);
    uint64_t act = ActiveConnections();

    std::string j;
    j += "{\"t\":";
    j += std::to_string(s.timestamp);
    j += ",\"qps\":";
    j += std::to_string(s.total.request_count);
    j += ",\"err\":";
    j += std::to_string(s.total.error_count);
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

std::string_view MetricsCollector::DashboardHtml()
{
    return R"HTML(<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HTTP Server Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,-apple-system,sans-serif;padding:24px}
h1{font-size:20px;margin-bottom:8px;color:#e94560}
.chart-grid{display:grid;grid-template-columns:1fr;gap:16px;max-width:1200px}
.chart-box{background:#16213e;border-radius:8px;padding:16px}
.chart-box h2{font-size:14px;color:#aaa;margin-bottom:8px}
.summary{display:flex;gap:24px;margin-bottom:16px;flex-wrap:wrap}
.stat{background:#16213e;border-radius:8px;padding:12px 20px;min-width:100px}
.stat-label{font-size:11px;color:#888;text-transform:uppercase}
.stat-value{font-size:24px;font-weight:600;color:#e94560;margin-top:2px}
.stat-value.up{color:#53d769}
.stat-value.warn{color:#ffa726}
.stat-value.danger{color:#e94560}
#alert-panel{margin-bottom:16px;display:none}
.alert-item{background:#16213e;border-left:3px solid #e94560;border-radius:4px;padding:8px 14px;margin-bottom:6px;font-size:13px}
.alert-item.ok{border-left-color:#53d769}
.alert-item .a-name{color:#e0e0e0;font-weight:600}
.alert-item .a-state{font-size:11px;margin-left:8px;padding:1px 6px;border-radius:3px}
.alert-item .a-state.firing{background:#e94560;color:#fff}
.alert-item .a-state.ok{background:#53d769;color:#fff}
</style>
</head>
<body>
<h1>⚡ HTTP Server Dashboard</h1>
<div id="alert-panel"></div>
<div class="summary">
  <div class="stat"><div class="stat-label">Requests/s</div><div class="stat-value up" id="cur-qps">—</div></div>
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
const MAX=60;
const labels=[],qpsData=[],errData=[],p50Data=[],p90Data=[],p99Data=[],actData=[];

const common={responsive:true,maintainAspectRatio:false,
  scales:{x:{display:false},y:{beginAtZero:true,grid:{color:'#2a2a4e'}}},
  plugins:{legend:{labels:{color:'#aaa',boxWidth:12,font:{size:11}}}}};

const QPS=new Chart(document.getElementById('chart-qps'),{type:'line',data:{
  labels,datasets:[
    {label:'QPS',data:qpsData,borderColor:'#53d769',backgroundColor:'rgba(83,215,105,0.1)',fill:true,pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'Errors',data:errData,borderColor:'#e94560',backgroundColor:'rgba(233,69,96,0.1)',fill:true,pointRadius:0,borderWidth:1.5,tension:.3}
  ]},options:common});

const LAT=new Chart(document.getElementById('chart-latency'),{type:'line',data:{
  labels,datasets:[
    {label:'p50',data:p50Data,borderColor:'#53d769',pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'p90',data:p90Data,borderColor:'#ffa726',pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'p99',data:p99Data,borderColor:'#e94560',pointRadius:0,borderWidth:1.5,tension:.3}
  ]},options:common});

const ACT=new Chart(document.getElementById('chart-connections'),{type:'line',data:{
  labels,datasets:[
    {label:'Active',data:actData,borderColor:'#42a5f5',backgroundColor:'rgba(66,165,245,0.15)',fill:true,pointRadius:0,borderWidth:2,tension:.3}
  ]},options:common});

// ── SSE: receive real-time metrics ──
//
// Server sends:
//   event: full\ndata: {history:[...],alerts:[...]}\n\n   (initial)
//   event: metrics\ndata: {t,qps,err,p50,p90,p99,act}\n\n (delta, ~1s interval)
//   event: alert\ndata: {name,state,value}\n\n            (on state transition)

function pushData(pt){
  labels.push(labels.length+'s');
  qpsData.push(pt.qps);errData.push(pt.err);
  p50Data.push(pt.p50);p90Data.push(pt.p90);p99Data.push(pt.p99);
  actData.push(pt.act);
  if(labels.length>MAX){ labels.shift();qpsData.shift();errData.shift();
    p50Data.shift();p90Data.shift();p99Data.shift();actData.shift(); }
  QPS.update();LAT.update();ACT.update();
}

function updateSummary(pt){
  document.getElementById('cur-qps').textContent=pt.qps;
  document.getElementById('cur-err').textContent=pt.err;
  document.getElementById('cur-lat').textContent=pt.p50+'/'+pt.p90+'/'+pt.p99+'µs';
  document.getElementById('cur-act').textContent=pt.act;
}

function updateAlerts(alerts){
  const panel=document.getElementById('alert-panel');
  if(!alerts||!alerts.length){panel.style.display='none';return;}
  panel.style.display='block';
  panel.innerHTML='';
  for(const a of alerts){
    const div=document.createElement('div');
    div.className='alert-item'+(a.state==='ok'?' ok':'');
    div.innerHTML='<span class="a-name">'+a.name+'</span>'+
      '<span class="a-state '+(a.state==='firing'?'firing':'ok')+'">'+
      a.state+'</span>';
    panel.appendChild(div);
  }
}

// Connect to SSE stream with configurable retry
let sinceTs=0;
const es=new EventSource('/metrics/stream');

es.addEventListener('full',function(e){
  try{
    const d=JSON.parse(e.data);
    const h=d.history||[];
    sinceTs=h.length>0?h[h.length-1].t:0;
    for(let i=0;i<h.length;i++)pushData(h[i]);
    if(h.length>0)updateSummary(h[h.length-1]);
    updateAlerts(d.alerts);
  }catch(x){console.error(x);}
});

es.addEventListener('metrics',function(e){
  try{
    const pt=JSON.parse(e.data);
    pushData(pt);
    updateSummary(pt);
    if(pt.t>sinceTs)sinceTs=pt.t;
  }catch(x){console.error(x);}
});

es.addEventListener('alert',function(e){
  try{
    const a=JSON.parse(e.data);
    // Re-fetch full alert list on any transition
    fetch('/metrics.json').then(r=>r.json()).then(d=>updateAlerts(d.alerts)).catch(()=>{});
  }catch(x){console.error(x);}
});

es.onerror=function(){
  // Browser auto-reconnects (EventSource handles this)
};
</script>
</body>
</html>)HTML";
}
