#pragma once
#include "http/context.hpp"
#include "net/session_region.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>

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
    int HeaderCount() const override { return header_count_; }
    std::pair<std::string_view, std::string_view> HeaderAt(int i) const override {
        if (i < 0 || i >= header_count_) return {};
        auto* r = Pool();
        return r ? std::pair{r->ToView(headers_[i].name), r->ToView(headers_[i].value)}
                 : std::pair<std::string_view, std::string_view>{};
    }

    /// True if the connection starts with HTTP/2 preface ("PRI * HTTP/2.0").
    bool IsH2() const { return h2_detected_; }

private:
    enum State : uint8_t {
        REQUEST_LINE,
        HEADERS,
        BODY,
        DONE,
        ERROR_STATE,
    };

    State state_ = REQUEST_LINE;

    static constexpr size_t kMaxLine = 4096;
    char line_buf_[kMaxLine];
    size_t line_len_ = 0;

    static constexpr int kMaxHeaders = 64;
    struct HeaderEntry { RegionOff name; RegionOff value; };
    HeaderEntry headers_[kMaxHeaders];
    int header_count_ = 0;

    std::string_view method_;
    std::string_view version_;
    RegionOff path_;
    RegionOff body_;
    size_t content_length_ = 0;
    size_t body_written_ = 0;

    bool h2_detected_ = false;
    bool message_complete_ = false;

    bool ProcessLine();
    bool ParseRequestLine();
    bool ParseHeaderLine();
    void WriteBody(const char* data, size_t len);
};
