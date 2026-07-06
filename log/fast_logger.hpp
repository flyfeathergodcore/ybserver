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

// ── 无锁日志系统 ──
//
// 每条线程一个 tls Queue（无锁），队列满或写线程要求刷新时移交。
// 专写线程每 1 秒递增版本号，线程在下一次 Log() 检测到变化后主动提交。
// 只在"移交"时刻竞争锁（每 4096 条/线程 或 每秒/线程）。
//
class FastLogger {
public:
    static FastLogger& Instance() {
        static FastLogger inst;
        return inst;
    }

    // 首次 Log() 自动启动写线程
    void Log(std::string msg) {
        if (!running_.load(std::memory_order_acquire))
            Start();

        auto* q = GetOrCreateQueue();
        q->entries.push_back(std::move(msg));

        // 若已停止（Stop 过程中），直接提交避免丢失
        if (q->Full() || !running_.load(std::memory_order_relaxed)) {
            SubmitQueue(q);
            return;
        }

        // 检测写线程的周期刷新请求
        uint64_t fe = flush_epoch_.load(std::memory_order_relaxed);
        if (fe != tls_epoch_) {
            tls_epoch_ = fe;
            if (!q->Empty())
                SubmitQueue(q);
        }
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!running_.exchange(false))
                return;
        }
        // 提交当前线程的 TLS 队列（避免队列中条目丢失）
        if (tls_q_ && !tls_q_->Empty())
            SubmitQueue(tls_q_);
        else if (tls_q_)
            tls_q_ = nullptr;  // 空队列，放弃持有

        cv_.notify_one();
        if (writer_.joinable())
            writer_.join();
        // 回收空闲队列
        {
            std::lock_guard<std::mutex> lock(mtx_);
            free_.clear();
        }
        if (file_.is_open())
            file_.close();
    }

    // 强制将所有线程活跃队列刷新到文件（测试用，同步等待）
    void Flush() {
        // 请求一次刷新
        uint64_t epoch = flush_epoch_.fetch_add(1, std::memory_order_relaxed) + 1;
        // 等写线程处理
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::seconds(2), [this, epoch] {
            return last_flushed_epoch_ >= epoch && ready_.empty();
        });
    }

private:
    struct Queue {
        static constexpr size_t MAX = 4096;
        static constexpr size_t MAX_FREE_QUEUES = 8;  // 最多保留 8 个空闲队列
        std::vector<std::string> entries;
        Queue() { entries.reserve(MAX); }
        bool Full() const { return entries.size() >= MAX; }
        bool Empty() const { return entries.empty(); }
    };

    FastLogger() = default;
    ~FastLogger() { Stop(); }
    FastLogger(const FastLogger&) = delete;
    FastLogger& operator=(const FastLogger&) = delete;

    void Start() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (running_.load()) return;

        std::filesystem::create_directory("logs");
        file_.open("logs/access.log", std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "[logger] 无法打开 logs/access.log" << std::endl;
            return;
        }

        running_.store(true, std::memory_order_release);
        writer_ = std::thread(&FastLogger::WriterLoop, this);

        // 等待写线程完成首次 epoch 递增
        while (!started_.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    Queue* GetOrCreateQueue() {
        if (!tls_q_) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!free_.empty()) {
                tls_q_ = free_.back().release();
                free_.pop_back();
            } else {
                tls_q_ = new Queue();
            }
        }
        return tls_q_;
    }

    void SubmitQueue(Queue* q) {
        std::lock_guard<std::mutex> lock(mtx_);
        ready_.push_back(q);
        tls_q_ = nullptr;      // 下次 Log 重新领空队列
        cv_.notify_one();
    }

    void WriterLoop() {
        std::vector<Queue*> local_ready;
        std::vector<Queue*> to_free;
        const auto period = std::chrono::seconds(1);

        // 首次递增 → 让 Start() 知道写线程已就绪
        flush_epoch_.fetch_add(1, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);

        for (;;) {
            // 通知各线程提交活跃队列
            flush_epoch_.fetch_add(1, std::memory_order_relaxed);

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait_for(lock, period, [this] {
                    return !ready_.empty() || !running_.load();
                });
                if (!running_.load() && ready_.empty())
                    break;
                local_ready.swap(ready_);
            }

            for (auto* q : local_ready) {
                for (const auto& e : q->entries)
                    file_ << e << '\n';
                q->entries.clear();
                q->entries.reserve(Queue::MAX);
                to_free.push_back(q);
            }
            local_ready.clear();
            file_.flush();

            if (!to_free.empty()) {
                std::lock_guard<std::mutex> lock(mtx_);
                // 限制空闲队列数量，超出部分直接释放
                while (free_.size() > Queue::MAX_FREE_QUEUES) {
                    free_.erase(free_.begin());
                }
                for (auto* q : to_free) {
                    if (free_.size() < Queue::MAX_FREE_QUEUES)
                        free_.emplace_back(q);
                    else
                        delete q;
                }
                to_free.clear();
            }

            // 记录已刷新版本号（给 Flush() 用）
            last_flushed_epoch_.store(
                flush_epoch_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
    }

    std::ofstream file_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> flush_epoch_{0};
    std::atomic<uint64_t> last_flushed_epoch_{0};

    std::vector<std::unique_ptr<Queue>> free_;
    std::vector<Queue*> ready_;
    std::thread writer_;

    static inline thread_local Queue* tls_q_ = nullptr;
    static inline thread_local uint64_t tls_epoch_ = 0;
};
