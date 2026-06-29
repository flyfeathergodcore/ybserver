#pragma once
#include "http/context.hpp"
#include "net/session_region.hpp"
#include <string_view>
#include <vector>

// ── H2StreamContext ──
//
// HTTP/2 stream state.  Implements the Context interface so middleware
// and handler can process requests identically to HTTP/1.1.
//
class H2StreamContext : public Context {
public:
    H2StreamContext() = default;

    void SetMethod(std::string_view m);
    void SetPath(std::string_view p);
    void AddHeader(std::string_view name, std::string_view value);
    void AppendBody(const uint8_t* data, size_t len);

    // ── Context interface ──
    ParseResult Feed(const char*, size_t) override { return ParseResult::Complete; }
    std::string_view Method()  const override;
    std::string_view Path()    const override;
    std::string_view Version() const override { return "HTTP/2"; }
    bool IsHttp2() const override { return true; }
    std::string_view Header(std::string_view key) const override;
    std::string_view Body()   const override;
    int HeaderCount() const override { return header_count_; }
    std::pair<std::string_view, std::string_view> HeaderAt(int i) const override {
        if (i < 0 || i >= header_count_) return {};
        auto* r = Pool();
        return r ? std::pair{r->ToView(headers_[i].name), r->ToView(headers_[i].value)}
                 : std::pair<std::string_view, std::string_view>{};
    }

    // ── Response body source (for nghttp2 DATA frames) ──
    const char* resp_body_ = nullptr;
    size_t resp_body_len_ = 0;
    size_t resp_body_off_ = 0;
    std::vector<char> file_buf_;   // for sendfile responses

    // ── SSE streaming state ──
    bool sse_active_ = false;
    int  push_interval_ms_ = 1000;
    std::string sse_payload_;

    // ── State ──
    bool handled_ = false;       // true when HandleStream has been spawned
    bool stream_closed_ = false; // true when RST_STREAM / stream close received

private:
    static constexpr int kMaxHeaders = 64;

    RegionOff method_;
    RegionOff path_;
    RegionOff body_;

    struct Entry { RegionOff name; RegionOff value; };
    Entry headers_[kMaxHeaders];
    int header_count_ = 0;
};
