#include "net/response.hpp"
#include "net/session_region.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>

// ── Region-backed response ──

Response::Response(int code, SessionRegion& region)
    : region_(&region)
    , begin_off_(region.Used())
    , code_(code)
{
    region_->Write("HTTP/1.1 ");
    switch (code) {
        case 200: region_->Write("200 OK"); break;
        case 206: region_->Write("206 Partial Content"); break;
        case 301: region_->Write("301 Moved Permanently"); break;
        case 302: region_->Write("302 Found"); break;
        case 307: region_->Write("307 Temporary Redirect"); break;
        case 308: region_->Write("308 Permanent Redirect"); break;
        case 204: region_->Write("204 No Content"); break;
        case 304: region_->Write("304 Not Modified"); break;
        case 101: region_->Write("101 Switching Protocols"); break;
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
    // Wire format — H1 reads this, H2 skips it entirely.
    if (!region_->StructuredMode()) {
        region_->Write(key);
        region_->Write(": ");
        region_->Write(value);
        region_->WriteCRLF();
    }

    // Structured headers — H2 reads this via HeaderAt().
    // H1 never enters this branch (StructuredMode is false).
    if (region_->StructuredMode()) {
        if (!hdr_)
            hdr_ = std::make_unique<HeaderStorage>();
        if (hdr_->header_count_ < kMaxHeaders) {
            auto& p = hdr_->pending_[hdr_->header_count_];
            p.name_len = static_cast<uint8_t>(std::min(key.size(), sizeof(p.name) - 1));
            std::memcpy(p.name, key.data(), p.name_len);
            p.value_len = static_cast<uint8_t>(std::min(value.size(),
                                                         sizeof(p.value) - 1));
            std::memcpy(p.value, value.data(), p.value_len);
            p.is_int = false;
            hdr_->header_count_++;
        }
    }
}

void Response::Header(std::string_view key, uint64_t value) {
    if (!region_->StructuredMode()) {
        region_->Write(key);
        region_->Write(": ");
        region_->WriteUint(value);
        region_->WriteCRLF();
    }

    if (region_->StructuredMode()) {
        if (!hdr_)
            hdr_ = std::make_unique<HeaderStorage>();
        if (hdr_->header_count_ < kMaxHeaders) {
            auto& p = hdr_->pending_[hdr_->header_count_];
            p.name_len = static_cast<uint8_t>(std::min(key.size(), sizeof(p.name) - 1));
            std::memcpy(p.name, key.data(), p.name_len);
            p.int_len = static_cast<uint8_t>(std::snprintf(
                reinterpret_cast<char*>(p.int_buf), sizeof(p.int_buf),
                "%lu", (unsigned long)value));
            p.is_int = true;
            hdr_->header_count_++;
        }
    }
}

// ── Cached HTTP-date (shared with h2 session) ──

std::string_view CachedDate()
{
    static time_t last = 0;
    static char buf[64];
    auto now = ::time(nullptr);
    if (now != last) {
        last = now;
        struct tm tm;
        ::gmtime_r(&now, &tm);
        ::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    }
    return {buf, strlen(buf)};
}

void Response::EndHeaders() {
    if (region_) {
        // Date + Connection — H1 wire only.
        // H2 reads these from structured storage if needed (Date added
        // explicitly in HandleStream; Connection is hop-by-hop for H2).
        if (!region_->StructuredMode()) {
            auto date = CachedDate();
            region_->Write("Date: ");
            region_->Write(date);
            region_->WriteCRLF();
            if (!sse_ && !ws_upgrade_)
                region_->Write("Connection: keep-alive\r\n");
        }
    }
    region_->WriteCRLF();
    header_end_ = region_->Used();

    // Structured headers: no DupOff to Region.  HeaderAt() reads directly
    // from hdr_->pending_[] inline buffers, which live until Response is
    // destroyed (after HandleStream finishes).
}

// ── Body ──

void Response::Body(const char* data, size_t len) {
    ext_body_ = data;
    ext_body_len_ = len;
}

void Response::BodyFile(int fd, size_t file_size,
                        size_t range_offset, size_t range_len) {
    fd_ = fd;
    file_size_ = file_size;
    file_range_offset_ = range_offset;
    file_range_len_ = range_len;
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

// ── Structured header access ──
//
// Reads directly from the heap-allocated pending_ buffers (H2 only).
// No DupOff/RegionOff needed — HeaderStorage lives until Response is
// destroyed, which outlives HandleStream's consumption.

int Response::HeaderCount() const {
    return hdr_ ? hdr_->header_count_ : 0;
}

std::pair<std::string_view, std::string_view> Response::HeaderAt(int i) const {
    if (!hdr_ || i < 0 || i >= hdr_->header_count_)
        return {};
    auto& p = hdr_->pending_[i];
    if (p.is_int)
        return {{p.name, p.name_len},
                {reinterpret_cast<const char*>(p.int_buf), p.int_len}};
    return {{p.name, p.name_len}, {p.value, p.value_len}};
}

// ── SSE stream factory ──

Response Response::SSEStream(SessionRegion& region, int min_interval_ms)
{
    Response resp(200, region);
    resp.sse_ = true;  // before EndHeaders so Date/Connection handled correctly
    resp.Header("Content-Type", "text/event-stream");
    resp.Header("Cache-Control", "no-cache");
    resp.EndHeaders();
    resp.push_interval_ms_ = std::max(min_interval_ms, 200);  // floor 200ms
    return resp;
}

// ── WebSocket upgrade factory ──

Response Response::WebSocketUpgrade(SessionRegion& region, std::string accept)
{
    Response resp(101, region);
    resp.ws_upgrade_ = true;
    resp.ws_accept_ = accept;
    resp.Header("Upgrade", "websocket");
    resp.Header("Connection", "Upgrade");
    resp.Header("Sec-WebSocket-Accept", std::move(accept));
    resp.EndHeaders();
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
        case 416: text = "Range Not Satisfiable"; break;
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
