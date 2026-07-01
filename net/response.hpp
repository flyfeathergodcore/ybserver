#pragma once
#include "net/session_region.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class SessionRegion;

class Response {
public:
    static constexpr int kMaxHeaders = 32;

    static Response None() { return {}; }
    Response(int code, SessionRegion& region);
    static Response Raw(int code, std::string wire);
    static Response Error(int code, SessionRegion& region);
    static Response SSEStream(SessionRegion& region, int min_interval_ms);

    // ── WebSocket upgrade response (101) ──
    static Response WebSocketUpgrade(SessionRegion& region,
                                      std::string accept);
    bool IsWebSocket() const { return ws_upgrade_; }
    std::string_view WsAccept() const { return ws_accept_; }

    // ── Header building ──
    void Header(std::string_view key, std::string_view value);
    void Header(std::string_view key, uint64_t value);
    void EndHeaders();

    // ── Structured header access (for H2, consumes what Header() stored) ──
    int HeaderCount() const;
    std::pair<std::string_view, std::string_view> HeaderAt(int i) const;

    // ── Body ──
    void Body(const char* data, size_t len);
    void Body(std::string_view s) { Body(s.data(), s.size()); }

    /// Serve a file by fd.  For partial-content (206) pass a non-zero
    /// range_offset/range_len — sendfile / pread will read only that portion.
    void BodyFile(int fd, size_t file_size,
                  size_t range_offset = 0, size_t range_len = 0);

    // ── Queries ──
    bool IsNone() const;
    bool IsFile() const;
    bool IsStream() const { return sse_; }
    int PushIntervalMs() const { return push_interval_ms_; }
    int StatusCode() const { return code_; }
    std::string_view HeaderWire() const;
    std::string_view BodyWire() const;
    int Fd() const { return fd_; }
    size_t FileSize() const { return file_size_; }
    size_t FileRangeOffset() const { return file_range_offset_; }
    size_t FileRangeLen() const { return file_range_len_; }

private:
    Response() = default;

    SessionRegion* region_ = nullptr;
    size_t begin_off_  = 0;
    size_t header_end_ = 0;
    int code_ = 200;

    static constexpr int kMaxFieldLen = 128;
    struct PendingHeader {
        char    name[kMaxFieldLen];
        uint8_t name_len = 0;
        char    value[kMaxFieldLen];
        uint8_t value_len = 0;
        uint8_t int_buf[24];       // formatted uint64_t
        uint8_t int_len   = 0;
        bool    is_int    = false;
    };

    // Heap-allocated header storage — only allocated when StructuredMode (H2).
    // H1 never hits this path, keeping sizeof(Response) = ~112 bytes.
    // HeaderAt() reads directly from these inline buffers (no RegionOff/DupOff).
    struct HeaderStorage {
        PendingHeader pending_[kMaxHeaders];
        int           header_count_ = 0;
    };
    std::unique_ptr<HeaderStorage> hdr_;

    const char* ext_body_ = nullptr;
    size_t ext_body_len_  = 0;
    int fd_ = -1;
    size_t file_size_ = 0;
    size_t file_range_offset_ = 0;
    size_t file_range_len_ = 0;

    std::string raw_wire_;
    bool raw_mode_ = false;

    bool sse_ = false;
    int push_interval_ms_ = 1000;

    bool ws_upgrade_ = false;
    std::string ws_accept_;
};
