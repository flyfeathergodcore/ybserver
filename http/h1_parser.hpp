#pragma once
#include "http/context.hpp"
#include "net/session_region.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

// ── H1Parser ──
//
// Hand-written HTTP/1.1 request parser — replaces llhttp (12,033 lines C).
//
// Design:
//  - Single line_buf_ (4 KB) accumulates one request/header line at a time
//  - No callbacks — direct Feed() state machine, switch-based
//  - Parsed fields written to SessionRegion via DupOff immediately on line
//    completion (no url_buf_ / cur_field_ / cur_value_ intermediate buffers)
//  - H2 preface detection merged in (removes need for Http2DetectMiddleware)
//
class H1Parser : public Context {
public:
    H1Parser();
    ~H1Parser() override;

    ParseResult Feed(const char* data, size_t len) override;

    std::string_view Method()  const override { return method_; }
    std::string_view Path()    const override;
    std::string_view Version() const override { return version_; }
    std::string_view Header(const std::string_view key) const override;
    std::string_view Body()   const override;

    /// True if the connection starts with HTTP/2 preface ("PRI * HTTP/2.0").
    bool IsH2() const { return h2_detected_; }

private:
    enum State : uint8_t {
        REQUEST_LINE,   ///< parsing "GET /path HTTP/1.1\r\n"
        HEADERS,        ///< parsing header lines
        BODY,           ///< copying body into pre-allocated region
        DONE,           ///< complete request parsed
        ERROR_STATE,    ///< parse error
    };

    State state_ = REQUEST_LINE;

    // ── Line buffer ──
    // Accumulates the current request line or header line until \r\n is seen.
    // After processing the line, the buffer is reset.
    static constexpr size_t kMaxLine = 4096;
    char line_buf_[kMaxLine];
    size_t line_len_ = 0;

    // ── Parsed results ──

    static constexpr int kMaxHeaders = 64;
    struct HeaderEntry { RegionOff name; RegionOff value; };
    HeaderEntry headers_[kMaxHeaders];
    int header_count_ = 0;

    std::string_view method_;     // "GET", "POST", ... (static storage)
    std::string_view version_;    // "HTTP/1.1", "HTTP/1.0" (static storage)
    RegionOff path_;
    RegionOff body_;
    size_t content_length_ = 0;
    size_t body_written_ = 0;

    bool h2_detected_ = false;
    bool message_complete_ = false;

    // ── Internal helpers ──

    /// Process a complete line (after \r\n). Returns false on parse error.
    bool ProcessLine();

    /// Parse "METHOD /path HTTP/1.1" from line_buf_.
    bool ParseRequestLine();

    /// Parse "key: value" from line_buf_.
    bool ParseHeaderLine();

    /// Copy body data into pre-allocated region space.
    void WriteBody(const char* data, size_t len);
};
