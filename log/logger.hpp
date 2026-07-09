#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>

// ═══════════════════════════════════════════════════════════════════
// 三层日志系统
//
// Gateway    — HTTP 请求日志 (logs/gateway.log)
// Business   — 业务日志 (logs/business.log)
// Perf       — 性能日志 (logs/perf.log)
// ═══════════════════════════════════════════════════════════════════

// ── 无锁异步日志写入器（每个分类一个） ──
class LogWriter {
    struct Chunk {
        static constexpr size_t MAX = 4096;
        std::vector<std::string> entries;
        Chunk() { entries.reserve(MAX); }
        bool Full() const { return entries.size() >= MAX; }
        bool Empty() const { return entries.empty(); }
    };

    std::string filename_;
    std::ofstream file_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false}, started_{false};
    std::atomic<uint64_t> flush_epoch_{0}, last_flushed_epoch_{0};
    std::vector<std::unique_ptr<Chunk>> free_;
    std::vector<Chunk*> ready_;
    std::thread writer_;
    static inline thread_local Chunk* tls_q_ = nullptr;
    static inline thread_local uint64_t tls_epoch_ = 0;

    void Start();
    Chunk* GetQueue();
    void Submit(Chunk* q);
    void Loop();

public:
    LogWriter(const std::string& filename) : filename_(filename) {}
    ~LogWriter() { Stop(); }
    void Write(std::string msg);
    void Stop();
};

// ═══════════════════════════════════════════════════════════════════
// LogWriter 实现
// ═══════════════════════════════════════════════════════════════════

inline void LogWriter::Start()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_.load()) return;
    std::filesystem::create_directory("logs");
    file_.open(filename_, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "[log] 无法打开 " << filename_ << std::endl;
        return;
    }
    running_.store(true, std::memory_order_release);
    writer_ = std::thread(&LogWriter::Loop, this);
    while (!started_.load(std::memory_order_acquire))
        std::this_thread::yield();
}

inline LogWriter::Chunk* LogWriter::GetQueue()
{
    if (!tls_q_) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!free_.empty()) {
            tls_q_ = free_.back().release();
            free_.pop_back();
        } else { tls_q_ = new Chunk(); }
    }
    return tls_q_;
}

inline void LogWriter::Submit(Chunk* q)
{
    std::lock_guard<std::mutex> lock(mtx_);
    ready_.push_back(q);
    tls_q_ = nullptr;
    cv_.notify_one();
}

inline void LogWriter::Loop()
{
    std::vector<Chunk*> local;
    std::vector<Chunk*> recycle;
    flush_epoch_.fetch_add(1, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
    for (;;) {
        flush_epoch_.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !ready_.empty() || !running_.load();
            });
            if (!running_.load() && ready_.empty()) break;
            local.swap(ready_);
        }
        for (auto* c : local) {
            for (auto& e : c->entries) file_ << e << '\n';
            c->entries.clear(); c->entries.reserve(Chunk::MAX);
            recycle.push_back(c);
        }
        local.clear(); file_.flush();
        if (!recycle.empty()) {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto* c : recycle)
                if (free_.size() < 8) free_.emplace_back(c); else delete c;
            recycle.clear();
        }
        last_flushed_epoch_.store(
            flush_epoch_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }
}

inline void LogWriter::Write(std::string msg)
{
    if (!running_.load(std::memory_order_acquire))
        Start();
    auto* q = GetQueue();
    q->entries.push_back(std::move(msg));
    if (q->Full() || !running_.load(std::memory_order_relaxed)) {
        Submit(q); return;
    }
    uint64_t fe = flush_epoch_.load(std::memory_order_relaxed);
    if (fe != tls_epoch_) {
        tls_epoch_ = fe;
        if (!q->Empty()) Submit(q);
    }
}

inline void LogWriter::Stop()
{
    { std::lock_guard<std::mutex> lock(mtx_);
      if (!running_.exchange(false)) return; }
    if (tls_q_ && !tls_q_->Empty()) Submit(tls_q_);
    else if (tls_q_) tls_q_ = nullptr;
    cv_.notify_one();
    if (writer_.joinable()) writer_.join();
    { std::lock_guard<std::mutex> lock(mtx_); free_.clear(); }
    if (file_.is_open()) file_.close();
}

// ═══════════════════════════════════════════════════════════════════
// Logger — 三层日志统一入口
// ═══════════════════════════════════════════════════════════════════

class Logger {
public:
    static Logger& Instance() { static Logger inst; return inst; }

    static std::string Timestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        auto t = system_clock::to_time_t(now);
        std::tm tm;
        localtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        char out[40];
        std::snprintf(out, sizeof(out), "%s.%03ld", buf, (long)ms.count());
        return out;
    }

    void Gateway(const std::string& method, const std::string& path,
                 int status, uint64_t latency_us,
                 const std::string& client_ip = "")
    {
        char buf[128];
        auto ms = latency_us / 1000.0;
        std::snprintf(buf, sizeof(buf), "[%s] %s %s %d %.1fms %s",
            Timestamp().c_str(), method.c_str(), path.c_str(),
            status, ms, client_ip.c_str());
        gw_.Write(buf);
    }

    void Business(const std::string& module, const std::string& action,
                  const std::string& detail)
    {
        std::string msg = "[" + Timestamp() + "] [" + module + "] "
                        + action + " " + detail;
        biz_.Write(msg);
    }

    void Perf(uint64_t total_req, uint64_t total_latency_us,
              int active_workers, int total_workers,
              uint64_t mem_kb)
    {
        double avg_ms = total_req > 0
            ? (total_latency_us / (double)total_req) / 1000.0 : 0.0;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "[%s] req=%llu avg=%.2fms workers=%d/%d mem=%lluKB",
            Timestamp().c_str(),
            (unsigned long long)total_req, avg_ms,
            active_workers, total_workers,
            (unsigned long long)mem_kb);
        perf_.Write(buf);
    }

    void StopAll() { gw_.Stop(); biz_.Stop(); perf_.Stop(); }

private:
    Logger() = default;
    ~Logger() { StopAll(); }
    Logger(const Logger&) = delete;
    LogWriter gw_{"logs/gateway.log"};
    LogWriter biz_{"logs/business.log"};
    LogWriter perf_{"logs/perf.log"};
};
