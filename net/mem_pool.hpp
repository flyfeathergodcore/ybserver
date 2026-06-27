#pragma once
#include <cstddef>
#include <memory>
#include <string_view>

// ── MemPool ──
//
// Simple arena allocator, one per session/connection.
// Allocations are bump-pointer fast.  Reset() reclaims
// all memory in one shot — ideal for per-request temporary
// data that lives until the end of a keep-alive iteration.
//
// Thread-compatible (not safe): external synchronization
// is the caller's responsibility.
//
class MemPool {
public:
    MemPool();
    ~MemPool();

    MemPool(const MemPool&) = delete;
    MemPool& operator=(const MemPool&) = delete;
    MemPool(MemPool&&) = delete;
    MemPool& operator=(MemPool&&) = delete;

    /// Allocate @a n bytes (8-aligned).  Returns nullptr on OOM.
    void* Alloc(size_t n);

    /// Allocate and copy a string_view into pool memory.
    /// Returns empty string_view on OOM.
    std::string_view Dup(std::string_view s);

    /// Reset the pool: all previously allocated memory is reclaimed.
    /// Keeps one block alive to avoid thrashing.
    void Reset();

    /// Total bytes allocated across all blocks (for stats / debugging).
    size_t TotalAllocated() const { return total_; }

    static constexpr size_t kBlockSize = 65536;   // 64 KB per block

private:
    struct Block {
        std::unique_ptr<char[]> data;
        size_t cap;        // total capacity
        size_t used;       // bytes consumed so far
        Block* next;       // linked list (older blocks)
    };

    Block* head_;
    size_t total_ = 0;     // cumulative allocated bytes (for stats)

    Block* NewBlock(size_t min_size);
};
