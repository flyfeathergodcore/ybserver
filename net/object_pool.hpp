#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>

// ═══════════════════════════════════════════════════════════════
// ObjectPool<T, Capacity>
//
// 通用对象复用池。Acquire() 从空闲链表取出对象（已调用 Reset()），
// 对象析构时自动归还。适合频繁构造/析构的对象：
//   - protobuf Message（ChatClientMessage、QueryRequest 等）
//   - grpc::ClientContext
//   - nlohmann::json
//
// 使用方式：
//   static ObjectPool<ai::chat::ChatClientMessage> pool;
//   auto msg = pool.Acquire();    // PooledObject<T> RAII 包装
//   msg->set_session_id("xxx");   // 用 -> 操作对象
//   // 离开作用域后自动 Clear() 归还
//
// 线程安全：spinlock 保护空闲栈，适合多 worker 共享同一个 pool。
//
// 对象必须满足以下接口之一（优先级从高到低）：
//   1. Reset()   — 自定义复位
//   2. Clear()   — protobuf Message 标准接口
//   3. clear()   — STL 容器标准接口
// ═══════════════════════════════════════════════════════════════

namespace detail {

// 探测复位函数：优先 Reset() → Clear() → clear()
template<typename T>
auto CallReset(T& obj, int) -> decltype(obj.Reset(), void()) { obj.Reset(); }

template<typename T>
auto CallReset(T& obj, long) -> decltype(obj.Clear(), void()) { obj.Clear(); }

template<typename T>
auto CallReset(T& obj, ...) -> decltype(obj.clear(), void()) { obj.clear(); }

} // namespace detail

template<typename T, size_t Capacity = 32>
class ObjectPool {
public:
    // RAII 句柄：析构时自动归还对象
    class PooledObject {
    public:
        PooledObject() = default;
        PooledObject(T* obj, ObjectPool* pool)
            : obj_(obj), pool_(pool) {}

        // 禁止拷贝
        PooledObject(const PooledObject&) = delete;
        PooledObject& operator=(const PooledObject&) = delete;

        // 允许移动
        PooledObject(PooledObject&& o) noexcept
            : obj_(o.obj_), pool_(o.pool_) {
            o.obj_ = nullptr;
        }
        PooledObject& operator=(PooledObject&& o) noexcept {
            if (this != &o) {
                release();
                obj_ = o.obj_;
                pool_ = o.pool_;
                o.obj_ = nullptr;
            }
            return *this;
        }

        ~PooledObject() { release(); }

        T* operator->() { return obj_; }
        T& operator*()  { return *obj_; }
        const T* operator->() const { return obj_; }
        const T& operator*()  const { return *obj_; }

        bool valid() const { return obj_ != nullptr; }

    private:
        void release() {
            if (obj_ && pool_) {
                detail::CallReset(*obj_, 0);
                pool_->Return(obj_);
                obj_ = nullptr;
            }
        }

        T*           obj_  = nullptr;
        ObjectPool*  pool_ = nullptr;
    };

    ObjectPool() = default;
    ~ObjectPool() {
        for (size_t i = 0; i < top_; i++) {
            delete stack_[i];
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // 从池中获取对象（池空则新建），返回 RAII 句柄
    PooledObject Acquire() {
        T* obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (top_ > 0) {
                obj = stack_[--top_];
            }
        }
        if (!obj) {
            obj = new T();
            created_.fetch_add(1, std::memory_order_relaxed);
        } else {
            reused_.fetch_add(1, std::memory_order_relaxed);
        }
        return {obj, this};
    }

    // 统计：已创建对象总数
    size_t Created() const { return created_.load(std::memory_order_relaxed); }

    // 统计：复用次数
    size_t Reused() const { return reused_.load(std::memory_order_relaxed); }

    // 当前空闲对象数
    size_t FreeCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return top_;
    }

private:
    void Return(T* obj) {
        std::lock_guard<std::mutex> lock(mu_);
        if (top_ < Capacity) {
            stack_[top_++] = obj;
        } else {
            // 池满，直接删除（防止无限增长）
            delete obj;
        }
    }

    mutable std::mutex        mu_;
    std::array<T*, Capacity>  stack_{};
    size_t                    top_ = 0;

    std::atomic<size_t> created_{0};
    std::atomic<size_t> reused_{0};
};

// ═══════════════════════════════════════════════════════════════
// ProtoArenaScope
//
// 将 protobuf Arena 绑定到 RegionPool 上，
// 让 protobuf message 直接在 RegionPool 的内存上分配。
//
// 使用方式：
//   ProtoArenaScope arena(region.Data(), 4096);
//   auto* req = arena.New<ai::chat::ChatRequest>();
//   req->set_session_id("xxx");
//   // arena 析构时统一释放，不涉及单个 message 的 malloc/free
//
// 注意：Arena 上的对象不能单独析构，必须随 Arena 整体释放。
// ═══════════════════════════════════════════════════════════════

#include <google/protobuf/arena.h>

class ProtoArenaScope {
public:
    // 在已有内存块上创建 Arena（使用 RegionPool 的内存）
    explicit ProtoArenaScope(void* initial_block, size_t block_size) {
        google::protobuf::ArenaOptions opts;
        opts.initial_block       = initial_block;
        opts.initial_block_size  = block_size;
        // 超出初始块时回退到 malloc（仍然比不用 Arena 好）
        arena_ = std::make_unique<google::protobuf::Arena>(opts);
    }

    // 在 Arena 上构造 protobuf message（零 malloc，分配在初始块里）
    template<typename T>
    T* New() {
        return google::protobuf::Arena::CreateMessage<T>(arena_.get());
    }

    google::protobuf::Arena* Get() { return arena_.get(); }

private:
    std::unique_ptr<google::protobuf::Arena> arena_;
};
