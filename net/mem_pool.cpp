#include "net/mem_pool.hpp"
#include <cstring>
#include <new>   // std::nothrow

// ── helpers ──

// Round up to 8-byte alignment.
static inline size_t Align8(size_t n) {
    return (n + 7) & ~size_t(7);
}

// ── ctor / dtor ──

MemPool::MemPool()
    : head_(nullptr)
{
    // First block is allocated lazily on first Alloc() call.
}

MemPool::~MemPool()
{
    Block* b = head_;
    while (b) {
        Block* next = b->next;
        // unique_ptr<char[]> frees the data array
        delete b;              // frees the Block struct itself
        b = next;
    }
}

// ── NewBlock ──

MemPool::Block* MemPool::NewBlock(size_t min_size)
{
    size_t cap = (min_size > kBlockSize) ? min_size : kBlockSize;
    auto  data = std::unique_ptr<char[]>(new (std::nothrow) char[cap]);
    if (!data) return nullptr;

    auto* block = new (std::nothrow) Block;
    if (!block) return nullptr;

    block->data = std::move(data);
    block->cap  = cap;
    block->used = 0;
    block->next = head_;   // prepend
    head_ = block;
    return block;
}

// ── Alloc ──

void* MemPool::Alloc(size_t n)
{
    if (n == 0) return nullptr;
    n = Align8(n);

    // Lazily create first block.
    if (!head_) {
        if (!NewBlock(n)) return nullptr;
    }

    // Fit in current block?
    if (head_->used + n > head_->cap) {
        // Need a new block.
        if (!NewBlock(n)) return nullptr;
    }

    void* ptr = head_->data.get() + head_->used;
    head_->used += n;
    total_ += n;
    return ptr;
}

// ── Dup ──

std::string_view MemPool::Dup(std::string_view s)
{
    if (s.empty()) return {};
    char* p = static_cast<char*>(Alloc(s.size()));
    if (!p) return {};
    std::memcpy(p, s.data(), s.size());
    return std::string_view(p, s.size());
}

// ── Reset ──

void MemPool::Reset()
{
    if (!head_) return;

    // Free all blocks except the first one (keep it warm).
    Block* keep = head_;
    Block* b    = keep->next;
    while (b) {
        Block* next = b->next;
        delete b;
        b = next;
    }
    keep->next = nullptr;
    keep->used = 0;
    head_ = keep;
    total_ = 0;
}
