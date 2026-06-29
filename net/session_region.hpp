#pragma once
#include "net/region_pool.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

// ── RegionOff ──
//
// Offset-based reference into a SessionRegion buffer.
// Survives migration (memcpy preserves relative offset).
//
struct RegionOff {
    uint32_t off = 0;
    uint32_t len = 0;

    bool IsValid() const { return len > 0; }
};

// ── SessionRegion ──
//
// Replacement for MemPool.  Each Session owns one SessionRegion.
// Backing memory comes from the Worker's RegionPool (a single 256 MB
// mmap), NOT from per-Session new/delete.
//
// Alloc / Dup bump-pointer from the region.  If the region runs out,
// Acquire a larger one from the pool, memcpy old data, Release the old
// one (vector-like growth).  All RegionOff references survive migration.
//
class SessionRegion {
public:
    SessionRegion() = default;
    ~SessionRegion();

    SessionRegion(const SessionRegion&) = delete;
    SessionRegion& operator=(const SessionRegion&) = delete;
    SessionRegion(SessionRegion&&) = delete;
    SessionRegion& operator=(SessionRegion&&) = delete;

    /// First-time init: attach to @a pool and acquire a 64 KB region.
    /// Idempotent — safe to call multiple times (re-acquires).
    void Init(RegionPool* pool);

    /// Reset for next request: set bump pointer back to 0.
    /// Does NOT release to pool (keeps region for reuse).
    void Reset();

    /// Bump allocate @a n bytes (8-aligned).
    void* Alloc(size_t n);

    /// Allocate + copy a string_view into region memory.
    std::string_view Dup(std::string_view s);

    /// Allocate + copy + return offset-based reference (survives migration).
    RegionOff DupOff(std::string_view s);

    /// Convert an offset+len pair to an absolute string_view.
    std::string_view ToView(RegionOff r) const {
        return {Data() + r.off, r.len};
    }

    char* Data() const {
        return pool_ ? pool_->Base() + offset_ : nullptr;
    }

    /// Current bump position (bytes used from Data()).
    size_t Used() const { return used_; }

    /// Bump-write a string_view into region (no alignment padding).
    void Write(std::string_view s);

    /// Shortcut: write "\r\n".
    void WriteCRLF();

    /// Write an unsigned integer as decimal (zero alloc).
    void WriteUint(uint64_t n);

    bool IsActive() const { return pool_ != nullptr && offset_ != 0; }

    /// Enable structured header mode (H2).  When set, Response::Header()
    /// stores structured kv pairs for nghttp2 consumption.  H1 keeps this
    /// false — zero overhead on the hot path.
    void SetStructuredMode(bool v) { structured_mode_ = v; }
    bool StructuredMode() const { return structured_mode_; }

    static constexpr size_t kInitSize = 65536;  // 64 KB initial
    static constexpr size_t kAlign     = 8;

private:
    RegionPool* pool_ = nullptr;
    size_t offset_ = 0;   // into pool_->Base()
    size_t cap_    = 0;   // current capacity of this region
    size_t used_   = 0;   // bump pointer
    bool structured_mode_ = false;

    /// Grow the region (vector-like: 2× capacity).
    void Migrate();
};
