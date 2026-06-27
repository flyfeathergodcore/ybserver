#pragma once
#include <string>
#include <string_view>
#include <sys/types.h>

class MemPool;   // forward decl

// ── Response ──
//
// Supports three modes:
//   1. None — empty sentinel (middleware passthrough).
//   2. String body — headers + body in memory (or pool-backed).
//   3. File body   — headers in memory, body from fd (sendfile).
//
class Response {
public:
    // ── Factories ──

    /// Sentinel: keep processing (no short-circuit).
    static Response None() { return Response(); }

    /// String-body response (default, heap-backed).
    Response(int code, std::string mime, std::string body);

    /// Error page shortcut.
    static Response Error(int code);

    /// File-body response.
    static Response File(int code, std::string mime,
                         int fd, size_t file_size);

    /// Raw wire-format response.
    static Response Raw(int code, std::string wire);

    /// Pool-allocated response.  Both headers and body memory come from
    /// @a pool (durable until the pool is Reset()).  AddHeader still
    /// works (falls back to std::string for the mutated header).
    static Response Pooled(MemPool& pool, int code,
                           std::string_view mime, std::string_view body);

    // ── Queries ──

    bool IsNone() const { return !valid_; }
    bool IsFile() const { return valid_ && fd_ >= 0; }
    bool IsString() const { return valid_ && fd_ < 0; }
    int StatusCode() const { return code_; }

    // ── Accessors ──

    /// HTTP headers (always present, always null-terminated).
    const std::string& Headers() const { return headers_; }

    /// Response body as a read-only view.
    /// For heap-backed responses this points to internal storage;
    /// for pool-backed responses it points into the MemPool.
    std::string_view Body() const;

    /// File descriptor (only for file responses).
    int Fd() const { return fd_; }
    size_t FileSize() const { return file_size_; }

    // ── Mutators (middleware) ──

    /// Insert a header *line* before the final "\r\n".
    void AddHeader(const std::string& line);

private:
    Response() = default;

    bool valid_ = false;
    std::string headers_;        // mutable headers
    std::string body_own_;       // owned body storage (heap-backed)
    std::string_view body_pool_; // pool-backed body view
    std::string mime_;
    MemPool* pool_ = nullptr;    // non-null when body_pool_ is active
    int fd_ = -1;
    size_t file_size_ = 0;
    int code_ = 200;
};
