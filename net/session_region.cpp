#include "net/session_region.hpp"
#include "net/region_pool.hpp"
#include <cstring>

SessionRegion::~SessionRegion() {
    if (pool_ && offset_ != 0) {
        pool_->Release(offset_, cap_);
    }
}

void SessionRegion::Init(RegionPool* pool) {
    // If already attached to this pool with a valid region, just reset.
    if (pool_ == pool && offset_ != 0) {
        used_ = 0;
        return;
    }

    // Release any previous region first.
    if (pool_ && offset_ != 0) {
        pool_->Release(offset_, cap_);
    }

    pool_ = pool;
    if (!pool_) return;

    auto [off, cap] = pool_->Acquire(kInitSize);
    offset_ = off;
    cap_    = cap;
    used_   = 0;
}

void SessionRegion::Reset() {
    used_ = 0;
    structured_mode_ = false;
}

void* SessionRegion::Alloc(size_t n) {
    if (n == 0) return nullptr;

    // Round to 8-byte alignment.
    n = (n + kAlign - 1) & ~(kAlign - 1);

    if (used_ + n > cap_) {
        Migrate();
        // Fallback: if still too small after migration, can't handle.
        if (used_ + n > cap_) return nullptr;
    }

    char* ptr = Data() + used_;
    used_ += n;
    return ptr;
}

std::string_view SessionRegion::Dup(std::string_view s) {
    if (s.empty()) return {};
    char* p = static_cast<char*>(Alloc(s.size()));
    if (!p) return {};
    std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
}

RegionOff SessionRegion::DupOff(std::string_view s) {
    auto sv = Dup(s);
    if (sv.empty()) return {};
    auto off = static_cast<uint32_t>(sv.data() - Data());
    return {off, static_cast<uint32_t>(sv.size())};
}

void SessionRegion::Migrate() {
    if (!pool_) return;

    // Vector-like: 2× capacity (at most one extra block per Session).
    size_t new_cap = cap_ * 2;
    if (new_cap > RegionPool::kPoolSize) new_cap = RegionPool::kPoolSize;

    auto [new_off, new_cap_actual] = pool_->Acquire(new_cap);
    if (new_off == 0) return;  // OOM

    // Copy old data to the start of the new region.
    if (used_ > 0) {
        std::memcpy(pool_->Base() + new_off, Data(), used_);
    }

    // Release old region.
    pool_->Release(offset_, cap_);

    // Switch.
    offset_ = new_off;
    cap_    = new_cap_actual;
}

void SessionRegion::Write(std::string_view s) {
    if (s.empty()) return;
    auto n = s.size();
    if (used_ + n > cap_) {
        Migrate();
        if (used_ + n > cap_) return;
    }
    std::memcpy(Data() + used_, s.data(), n);
    used_ += n;
}

void SessionRegion::WriteCRLF() {
    Write("\r\n");
}

void SessionRegion::WriteUint(uint64_t n) {
    char tmp[24];
    char* p = tmp + sizeof(tmp);
    *--p = '\0';
    if (n == 0) { *--p = '0'; }
    else {
        while (n > 0) {
            *--p = char('0' + (n % 10));
            n /= 10;
        }
    }
    Write(p);
}
