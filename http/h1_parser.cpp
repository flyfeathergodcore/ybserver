#include "http/h1_parser.hpp"
#include <cctype>
#include <cstring>

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

H1Parser::H1Parser() = default;
H1Parser::~H1Parser() = default;

// ═══════════════════════════════════════════════════════════════
// Feed — main state machine
// ═══════════════════════════════════════════════════════════════

ParseResult H1Parser::Feed(const char* data, size_t len)
{
    // ── H2 preface detection (first call only) ──
    if (state_ == REQUEST_LINE && line_len_ == 0 && len >= 15) {
        static constexpr char PREFACE[] = "PRI * HTTP/2.0\r\n";
        if (std::memcmp(data, PREFACE, 15) == 0) {
            h2_detected_ = true;
            return ParseResult::Error;
        }
    }

    // ── Per-request reset (keep-alive or pool reuse) ──
    if (state_ == DONE || state_ == ERROR_STATE) {
        state_ = REQUEST_LINE;
        message_complete_ = false;
        header_count_   = 0;
        line_len_       = 0;
        content_length_ = 0;
        body_written_   = 0;
    }

    ClearResponseHeaders();

    size_t pos = 0;
    while (pos < len)
    {
        if (state_ == REQUEST_LINE || state_ == HEADERS)
        {
            // ── Try to find \r\n in the remaining input ──
            const char* cr = static_cast<const char*>(
                std::memchr(data + pos, '\r', len - pos));

            bool crlf_found = (cr != nullptr)
                           && (static_cast<size_t>(cr + 1 - data) < len)
                           && cr[1] == '\n';

            if (!crlf_found)
            {
                // ── Check for \r\n split across buffer boundary ──
                //   Previous call buffered "..\r" (line_buf_ ends with \r)
                //   This call starts with "\n"
                bool split = (line_len_ > 0 && line_buf_[line_len_ - 1] == '\r'
                              && len > pos && data[pos] == '\n');

                if (!split)
                {
                    // Buffer everything, wait for more data
                    size_t space = kMaxLine - 1 - line_len_;
                    size_t copy = std::min(space, len - pos);
                    if (copy == 0) { state_ = ERROR_STATE; return ParseResult::Error; }
                    std::memcpy(line_buf_ + line_len_, data + pos, copy);
                    line_len_ += copy;
                    pos += copy;
                    break;  // need more data
                }

                // ── Split \r\n case ──
                //   line_buf_ = "...content\r"  →  replace \r with \0
                //   data[pos] = '\n'             →  skip
                line_buf_[line_len_ - 1] = '\0';
                if (!ProcessLine()) {
                    state_ = ERROR_STATE; return ParseResult::Error;
                }
                line_len_ = 0;
                pos++;  // skip \n
                if (state_ == BODY || state_ == DONE || state_ == ERROR_STATE)
                    break;
                continue;
            }

            // ── Complete line found ──
            size_t line_data_len = static_cast<size_t>(cr - (data + pos));
            size_t total_len = line_len_ + line_data_len;

            if (total_len >= kMaxLine - 1)
                { state_ = ERROR_STATE; return ParseResult::Error; }

            if (line_data_len > 0)
                std::memcpy(line_buf_ + line_len_, data + pos, line_data_len);
            total_len = line_len_ + line_data_len;
            line_buf_[total_len] = '\0';

            if (!ProcessLine())
            {
                state_ = ERROR_STATE; return ParseResult::Error;
            }

            line_len_ = 0;
            pos = static_cast<size_t>(cr - data) + 2;  // skip \r\n

            if (state_ == BODY || state_ == DONE || state_ == ERROR_STATE)
                break;
            continue;
        }

        if (state_ == BODY)
        {
            size_t needed = content_length_ - body_written_;
            size_t avail  = len - pos;
            size_t copy   = std::min(needed, avail);
            WriteBody(data + pos, copy);
            pos += copy;
            if (body_written_ >= content_length_)
            {
                state_ = DONE;
                message_complete_ = true;
            }
            continue;
        }

        // DONE or ERROR — stop consuming
        break;
    }

    if (state_ == DONE)     return ParseResult::Complete;
    if (state_ == ERROR_STATE) return ParseResult::Error;
    return ParseResult::Incomplete;
}

// ═══════════════════════════════════════════════════════════════
// Line processing
// ═══════════════════════════════════════════════════════════════

bool H1Parser::ProcessLine()
{
    if (state_ == REQUEST_LINE)
        return ParseRequestLine();

    if (state_ == HEADERS)
    {
        // Empty line → end of headers
        if (line_len_ == 0)
        {
            auto* pool = Pool();
            if (content_length_ > 0 && pool)
            {
                void* buf = pool->Alloc(content_length_);
                if (buf)
                {
                    auto off = static_cast<uint32_t>(
                        static_cast<char*>(buf) - pool->Data());
                    body_ = {off, static_cast<uint32_t>(content_length_)};
                }
            }
            state_ = (content_length_ > 0) ? BODY : DONE;
            if (content_length_ == 0) message_complete_ = true;
            return true;
        }
        return ParseHeaderLine();
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
// Parsers
// ═══════════════════════════════════════════════════════════════

bool H1Parser::ParseRequestLine()
{
    // line_buf_ = "GET /path HTTP/1.1"
    char* p = line_buf_;

    // Method
    char* space = std::strchr(p, ' ');
    if (!space) return false;
    *space = '\0';
    if      (std::strcmp(p, "GET")     == 0) method_ = "GET";
    else if (std::strcmp(p, "POST")    == 0) method_ = "POST";
    else if (std::strcmp(p, "PUT")     == 0) method_ = "PUT";
    else if (std::strcmp(p, "DELETE")  == 0) method_ = "DELETE";
    else if (std::strcmp(p, "HEAD")    == 0) method_ = "HEAD";
    else if (std::strcmp(p, "OPTIONS") == 0) method_ = "OPTIONS";
    else return false;

    // Path
    p = space + 1;
    space = std::strchr(p, ' ');
    if (!space) return false;
    *space = '\0';

    if (auto* pool = Pool())
        path_ = pool->DupOff({p, static_cast<size_t>(space - p)});

    // Version
    p = space + 1;
    if      (std::strcmp(p, "HTTP/1.1") == 0) version_ = "HTTP/1.1";
    else if (std::strcmp(p, "HTTP/1.0") == 0) version_ = "HTTP/1.0";
    else                                       version_ = "HTTP/1.1";

    state_ = HEADERS;
    return true;
}

bool H1Parser::ParseHeaderLine()
{
    // line_buf_ = "key: value"
    char* colon = std::strchr(line_buf_, ':');
    if (!colon) return false;

    *colon = '\0';
    char* name  = line_buf_;
    char* value = colon + 1;
    while (*value == ' ') value++;  // trim leading spaces

    if (header_count_ >= kMaxHeaders)
    {
        // Too many headers — skip but don't error
        *colon = ':';  // restore (harmless since we're skipping)
        return true;
    }

    auto* pool = Pool();
    if (pool)
    {
        // Lowercase the key for case-insensitive lookup
        for (char* c = name; *c; c++)
            *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));

        headers_[header_count_].name  = pool->DupOff({name, std::strlen(name)});
        headers_[header_count_].value = pool->DupOff({value, std::strlen(value)});
        header_count_++;

        // Parse Content-Length
        if (std::strcmp(name, "content-length") == 0)
        {
            content_length_ = 0;
            for (char* c = value; *c; c++)
            {
                if (*c < '0' || *c > '9') break;
                content_length_ = content_length_ * 10
                                + static_cast<size_t>(*c - '0');
            }
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
// Body
// ═══════════════════════════════════════════════════════════════

void H1Parser::WriteBody(const char* data, size_t len)
{
    if (len == 0 || body_.len == 0) return;
    auto* pool = Pool();
    if (!pool) return;

    auto remain = static_cast<size_t>(body_.len) - body_written_;
    auto copy   = std::min(len, remain);
    if (copy == 0) return;
    std::memcpy(pool->Data() + body_.off + body_written_, data, copy);
    body_written_ += copy;
}

// ═══════════════════════════════════════════════════════════════
// Accessors
// ═══════════════════════════════════════════════════════════════

std::string_view H1Parser::Path() const
{
    auto* r = Pool();
    return r ? r->ToView(path_) : std::string_view{};
}

std::string_view H1Parser::Body() const
{
    auto* r = Pool();
    return r ? r->ToView(body_) : std::string_view{};
}

std::string_view H1Parser::Header(const std::string_view key) const
{
    auto* r = Pool();
    if (!r) return {};
    for (int i = 0; i < header_count_; i++)
    {
        if (r->ToView(headers_[i].name) == key)
            return r->ToView(headers_[i].value);
    }
    return {};
}
