#include "net/response.hpp"
#include "net/session_region.hpp"
#include <cstdio>

// ── Region-backed response ──

Response::Response(int code, SessionRegion& region)
    : region_(&region)
    , begin_off_(region.Used())
    , code_(code)
{
    // Write status line directly to region
    region_->Write("HTTP/1.1 ");
    switch (code) {
        case 200: region_->Write("200 OK"); break;
        case 204: region_->Write("204 No Content"); break;
        case 400: region_->Write("400 Bad Request"); break;
        case 403: region_->Write("403 Forbidden"); break;
        case 404: region_->Write("404 Not Found"); break;
        case 426: region_->Write("426 Upgrade Required"); break;
        case 501: region_->Write("501 Not Implemented"); break;
        default:  region_->Write("500 Internal Server Error"); break;
    }
    region_->WriteCRLF();
}

// ── Raw (pre-built wire, no region) ──

Response Response::Raw(int code, std::string wire)
{
    Response r;
    r.code_ = code;
    r.raw_wire_ = std::move(wire);
    r.raw_mode_ = true;
    return r;
}

// ── Header building ──

void Response::Header(std::string_view key, std::string_view value) {
    region_->Write(key);
    region_->Write(": ");
    region_->Write(value);
    region_->WriteCRLF();
}

void Response::Header(std::string_view key, uint64_t value) {
    region_->Write(key);
    region_->Write(": ");
    region_->WriteUint(value);
    region_->WriteCRLF();
}

void Response::EndHeaders() {
    region_->WriteCRLF();
    header_end_ = region_->Used();
}

// ── Body ──

void Response::Body(const char* data, size_t len) {
    ext_body_ = data;
    ext_body_len_ = len;
}

void Response::BodyFile(int fd, size_t file_size) {
    fd_ = fd;
    file_size_ = file_size;
}

// ── Queries ──

bool Response::IsNone() const {
    return !region_ && !raw_mode_;
}

bool Response::IsFile() const {
    return region_ && fd_ >= 0;
}

std::string_view Response::HeaderWire() const {
    if (raw_mode_) return raw_wire_;
    if (region_)
        return {region_->Data() + begin_off_, header_end_ - begin_off_};
    return {};
}

std::string_view Response::BodyWire() const {
    if (ext_body_)     // external body (FileCache, not in region)
        return {ext_body_, ext_body_len_};
    if (region_ && header_end_ > 0 && region_->Used() > header_end_)
        return {region_->Data() + header_end_, region_->Used() - header_end_};
    return {};
}

// ── SSE stream factory ──

Response Response::SSEStream(SessionRegion& region, int min_interval_ms)
{
    Response resp(200, region);
    resp.Header("Content-Type", "text/event-stream");
    resp.Header("Cache-Control", "no-cache");
    resp.Header("Connection", "keep-alive");
    resp.EndHeaders();
    resp.sse_ = true;
    resp.push_interval_ms_ = std::max(min_interval_ms, 200);  // floor 200ms
    return resp;
}

// ── Error factory ──

Response Response::Error(int code, SessionRegion& region)
{
    const char* text;
    switch (code) {
        case 400: text = "Bad Request"; break;
        case 403: text = "Forbidden"; break;
        case 404: text = "Not Found"; break;
        case 501: text = "Not Implemented"; break;
        default:  text = "Error"; break;
    }

    char body[128];
    int body_len = std::snprintf(body, sizeof(body),
                                 "<h1>%d %s</h1>", code, text);
    if (body_len < 0) body_len = 0;

    Response resp(code, region);
    resp.Header("Content-Type", "text/html");
    resp.Header("Content-Length", static_cast<uint64_t>(body_len));
    resp.EndHeaders();
    region.Write({body, static_cast<size_t>(body_len)});
    return resp;
}
