#include "http/llhttp_parser.hpp"
#include <algorithm>
#include <cctype>

static LlhttpParser* Self(llhttp_t* p) {
    return static_cast<LlhttpParser*>(p->data);
}

// ── Callbacks ──

int LlhttpParser::OnUrl(llhttp_t* p, const char* at, size_t len) {
    Self(p)->path_.append(at, len);
    return 0;
}

int LlhttpParser::OnHeaderField(llhttp_t* p, const char* at, size_t len) {
    Self(p)->last_header_field_.append(at, len);
    return 0;
}

int LlhttpParser::OnHeaderValue(llhttp_t* p, const char* at, size_t len) {
    auto* self = Self(p);
    std::string key = self->last_header_field_;
    for (auto& c : key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    self->headers_[std::move(key)] = std::string(at, len);
    self->last_header_field_.clear();
    return 0;
}

int LlhttpParser::OnHeadersComplete(llhttp_t* p) {
    auto* self = Self(p);
    switch (llhttp_get_method(p)) {
        case HTTP_GET:     self->method_ = "GET"; break;
        case HTTP_POST:    self->method_ = "POST"; break;
        case HTTP_PUT:     self->method_ = "PUT"; break;
        case HTTP_DELETE:  self->method_ = "DELETE"; break;
        case HTTP_HEAD:    self->method_ = "HEAD"; break;
        case HTTP_OPTIONS: self->method_ = "OPTIONS"; break;
        default:           self->method_ = "UNKNOWN"; break;
    }
    int maj = llhttp_get_http_major(p);
    int min = llhttp_get_http_minor(p);
    self->version_ = "HTTP/" + std::to_string(maj) + "." + std::to_string(min);
    return 0;
}

int LlhttpParser::OnBody(llhttp_t* p, const char* at, size_t len) {
    Self(p)->body_.append(at, len);
    return 0;
}

int LlhttpParser::OnMessageComplete(llhttp_t* p) {
    Self(p)->message_complete_ = true;
    return 0;
}

// ── Public API ──

LlhttpParser::LlhttpParser()
{
    llhttp_settings_init(&settings_);
    settings_.on_url              = OnUrl;
    settings_.on_header_field     = OnHeaderField;
    settings_.on_header_value     = OnHeaderValue;
    settings_.on_headers_complete = OnHeadersComplete;
    settings_.on_body             = OnBody;
    settings_.on_message_complete = OnMessageComplete;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

LlhttpParser::~LlhttpParser()
{
    // llhttp requires no explicit cleanup
}

ParseResult LlhttpParser::Feed(const char* data, size_t len)
{
    // Reset per-request state
    llhttp_reset(&parser_);
    method_.clear();
    path_.clear();
    version_.clear();
    headers_.clear();
    body_.clear();
    last_header_field_.clear();
    error_reason_.clear();
    message_complete_ = false;

    llhttp_errno_t err = llhttp_execute(&parser_, data, len);
    if (err != HPE_OK && err != HPE_PAUSED) {
        error_reason_ = llhttp_errno_name(err);
        return ParseResult::Error;
    }

    if (!message_complete_)
        return ParseResult::Incomplete;

    return ParseResult::Complete;
}

std::string_view LlhttpParser::Header(const std::string_view key) const
{
    auto it = headers_.find(std::string(key));
    return it != headers_.end() ? it->second : std::string_view();
}
