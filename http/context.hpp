#pragma once
#include <string>
#include <string_view>

enum class ParseResult { Incomplete = 0, Complete = 1, Error = -1 };

class SessionRegion;   // forward decl

class Context {
public:
    virtual ~Context() = default;

    // Feed raw data into the parser
    virtual ParseResult Feed(const char* data, size_t len) = 0;

    // Access parsed results
    virtual std::string_view Method()  const = 0;
    virtual std::string_view Path()    const = 0;
    virtual std::string_view Version() const = 0;
    virtual std::string_view Header(const std::string_view key) const = 0;
    virtual std::string_view Body()   const = 0;

    // Header enumeration (for proxy forwarding etc.)
    virtual int HeaderCount() const = 0;
    virtual std::pair<std::string_view, std::string_view> HeaderAt(int i) const = 0;

    // Protocol identification (for per-protocol metrics).
    virtual bool IsHttp2() const { return false; }

    // Per-request memory pool (set by Session before Feed).
    SessionRegion* Pool() const { return pool_; }
    void SetPool(SessionRegion* p) const { pool_ = p; }

    // ── Response header injection (middleware → handler) ──
    //
    // Middleware calls AddResponseHeader() before next.Handle().
    // Handler reads injected headers via ResponseHeaders() and
    // writes them to the region.  Cleared at the start of each Feed().
    //
    static constexpr int kMaxExtraHeaders = 8;

    void AddResponseHeader(std::string_view key,
                           std::string_view value) const {
        if (extra_header_count_ >= kMaxExtraHeaders) return;
        extra_header_keys_[extra_header_count_] = key;
        extra_header_vals_[extra_header_count_] = value;
        extra_header_count_++;
    }
    int ResponseHeaderCount() const { return extra_header_count_; }
    std::string_view ResponseHeaderKey(int i) const { return extra_header_keys_[i]; }
    std::string_view ResponseHeaderVal(int i) const { return extra_header_vals_[i]; }
    void ClearResponseHeaders() const {
        extra_header_count_ = 0;
    }

    // ── X-Request-Id ──
    void SetRequestId(std::string_view id) const { request_id_ = id; }
    std::string_view RequestId() const { return request_id_; }

private:
    mutable SessionRegion* pool_ = nullptr;

    mutable int extra_header_count_ = 0;
    mutable std::string_view extra_header_keys_[kMaxExtraHeaders];
    mutable std::string_view extra_header_vals_[kMaxExtraHeaders];
    mutable std::string_view request_id_;
};
