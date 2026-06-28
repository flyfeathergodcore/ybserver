#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class SessionRegion;

// ── Response ──
//
// All response data goes to the SessionRegion:
//
//   [status line\r\n][headers\r\n]\r\n[body...]
//   ↑                                   ↑
//   begin_off_                          region->Used()
//
// Body is either INLINE (written to region after \r\n),
// EXTERNAL (pointer to FileCache mmap), or FILE (sendfile).
//
class Response {
public:
    // ── Factories ──

    /// Sentinel: keep processing (no short-circuit).
    static Response None() { return {}; }

    /// Start building — writes status line to @a region immediately.
    /// @a region must outlive the Response (it's just a view).
    Response(int code, SessionRegion& region);

    /// Pre-built wire (for middleware ProcessRaw path, no region needed).
    static Response Raw(int code, std::string wire);

    /// Error page (text/html, written to region).
    static Response Error(int code, SessionRegion& region);

    /// SSE stream marker: headers written, Session::Send() enters SSE loop.
    /// @a min_interval_ms minimum time between pushes (guard against flood).
    static Response SSEStream(SessionRegion& region, int min_interval_ms);

    // ── Header building (writes directly to region) ──

    void Header(std::string_view key, std::string_view value);
    void Header(std::string_view key, uint64_t value);

    /// Finalize headers (write "\r\n", record position).
    void EndHeaders();

    // ── Body ──

    /// External body (from FileCache, not copied into region).
    void Body(const char* data, size_t len);
    void Body(std::string_view s) { Body(s.data(), s.size()); }

    /// File body (sendfile).
    void BodyFile(int fd, size_t file_size);

    // ── Queries (for Session::Send) ──

    bool IsNone() const;
    bool IsFile() const;
    bool IsStream() const { return sse_; }

    /// Minimum push interval for SSE (milliseconds).
    int PushIntervalMs() const { return push_interval_ms_; }

    /// HTTP status code (for metrics / logging).
    int StatusCode() const { return code_; }

    /// Headers wire (region or raw).
    std::string_view HeaderWire() const;

    /// Body wire — either external pointer, region content after headers, or empty.
    std::string_view BodyWire() const;

    /// File descriptor + size (for IsFile() path).
    int Fd() const { return fd_; }
    size_t FileSize() const { return file_size_; }

private:
    Response() = default;

    SessionRegion* region_ = nullptr;
    size_t begin_off_  = 0;   // where status line starts in region
    size_t header_end_ = 0;   // past the final \r\n (before body)
    int code_ = 200;

    // Body sources
    const char* ext_body_ = nullptr;
    size_t ext_body_len_  = 0;
    int fd_ = -1;
    size_t file_size_ = 0;

    // Raw mode (pre-built wire, no region)
    std::string raw_wire_;
    bool raw_mode_ = false;

    // SSE mode
    bool sse_ = false;
    int push_interval_ms_ = 1000;
};
