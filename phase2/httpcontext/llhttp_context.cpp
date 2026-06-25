#include "llhttp_context.hpp"
#include <algorithm>
#include <cctype>

static LlhttpContext* Self(llhttp_t* p) {
    return static_cast<LlhttpContext*>(p->data);
}

// ── 回调 ──
int LlhttpContext::OnUrl(llhttp_t* p, const char* at, size_t len) {
    Self(p)->path_.append(at, len);
    return 0;
}

int LlhttpContext::OnHeaderField(llhttp_t* p, const char* at, size_t len) {
    auto* self = Self(p);
    // 上一个 value 已处理完，开始新 field
    if (!self->last_header_field_.empty()) {
        // 如果是上一行 continuation
    }
    self->last_header_field_.append(at, len);
    return 0;
}

int LlhttpContext::OnHeaderValue(llhttp_t* p, const char* at, size_t len) {
    auto* self = Self(p);
    // key 转小写
    std::string key = self->last_header_field_;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    self->headers_[std::move(key)] = std::string(at, len);
    self->last_header_field_.clear();
    return 0;
}

int LlhttpContext::OnHeadersComplete(llhttp_t* p) {
    auto* self = Self(p);
    self->headers_complete_ = true;

    // 获取 method
    switch (llhttp_get_method(p)) {
        case HTTP_GET:     self->method_ = "GET"; break;
        case HTTP_POST:    self->method_ = "POST"; break;
        case HTTP_PUT:     self->method_ = "PUT"; break;
        case HTTP_DELETE:  self->method_ = "DELETE"; break;
        case HTTP_HEAD:    self->method_ = "HEAD"; break;
        default:           self->method_ = "UNKNOWN"; break;
    }

    // 获取 version
    int maj = llhttp_get_http_major(p);
    int min = llhttp_get_http_minor(p);
    self->version_ = "HTTP/" + std::to_string(maj) + "." + std::to_string(min);

    return 0;  // 返回 HPE_OK
}

int LlhttpContext::OnBody(llhttp_t* p, const char* at, size_t len) {
    Self(p)->body_.append(at, len);
    return 0;
}

int LlhttpContext::OnMessageComplete(llhttp_t* p) {
    Self(p)->message_complete_ = true;
    return 0;
}

// ── 公有方法 ──

LlhttpContext::LlhttpContext()
{
    Reset();
}

void LlhttpContext::Reset()
{
    method_.clear();
    path_.clear();
    version_.clear();
    headers_.clear();
    body_.clear();
    last_header_field_.clear();
    headers_complete_ = false;
    message_complete_ = false;
    error_reason_.clear();

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

ParseResult LlhttpContext::Feed(const char* data, size_t len)
{
    Reset();

    llhttp_errno_t err = llhttp_execute(&parser_, data, len);
    if (err != HPE_OK && err != HPE_PAUSED) {
        error_reason_ = llhttp_errno_name(err);
        return ParseResult::Error;
    }

    if (!message_complete_)
        return ParseResult::Incomplete;

    return ParseResult::Complete;
}

std::string_view LlhttpContext::Header(const std::string_view key) const
{
    auto it = headers_.find(std::string(key));
    return it != headers_.end() ? it->second : std::string_view();
}

std::string LlhttpContext::MakeResponse(
    int code, const std::string& mime, const std::string& body) const
{
    std::string status =
        (code == 200) ? "200 OK" :
        (code == 404) ? "404 Not Found" :
        (code == 403) ? "403 Forbidden" :
        (code == 400) ? "400 Bad Request" :
                        "501 Not Implemented";

    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + mime + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: keep-alive\r\n"
           "\r\n" + body;
}

std::string LlhttpContext::MakeError(int code) const
{
    std::string msg =
        (code == 404) ? "Not Found" :
        (code == 403) ? "Forbidden" :
        (code == 400) ? "Bad Request" :
        (code == 501) ? "Not Implemented" :
                        "Error";
    std::string body = "<h1>" + std::to_string(code) + " " + msg + "</h1>";
    return MakeResponse(code, "text/html", body);
}
