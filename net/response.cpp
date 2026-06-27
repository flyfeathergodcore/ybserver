#include "net/response.hpp"
#include "net/mem_pool.hpp"
#include "http/response.hpp"
#include "http/fixed_buffer.hpp"
#include <unistd.h>

// ── String-body response (heap-backed) ──

Response::Response(int code, std::string mime, std::string body)
    : valid_(true)
    , body_own_(std::move(body))
    , mime_(std::move(mime))
    , code_(code)
{
    FixedBuffer buf;
    http::BuildHeaderTo(buf, code_, mime_, body_own_.size());
    headers_.assign(buf.Data(), buf.Size());
}

// ── Error response ──

Response Response::Error(int code)
{
    std::string_view text =
        (code == 400) ? "Bad Request" :
        (code == 403) ? "Forbidden" :
        (code == 404) ? "Not Found" :
        (code == 501) ? "Not Implemented" :
                        "Error";
    std::string body = "<h1>" + std::to_string(code) + " "
                       + std::string(text) + "</h1>";
    return Response(code, "text/html", std::move(body));
}

// ── File-body response ──

Response Response::File(int code, std::string mime,
                         int fd, size_t file_size)
{
    Response r;
    r.valid_ = true;
    r.code_ = code;
    r.fd_ = fd;
    r.file_size_ = file_size;
    r.mime_ = std::move(mime);
    FixedBuffer buf;
    http::BuildHeaderTo(buf, r.code_, r.mime_, file_size);
    r.headers_.assign(buf.Data(), buf.Size());
    return r;
}

// ── Raw wire-format response ──

Response Response::Raw(int code, std::string wire)
{
    Response r;
    r.valid_ = true;
    r.code_ = code;
    r.headers_ = std::move(wire);
    return r;
}

// ── Pool-allocated response ──

Response Response::Pooled(MemPool& pool, int code,
                           std::string_view mime, std::string_view body)
{
    Response r;
    r.valid_ = true;
    r.code_ = code;
    r.pool_ = &pool;

    // Headers from pool
    FixedBuffer buf;
    http::BuildHeaderTo(buf, code, mime, body.size());
    auto hdr = pool.Dup(buf.View());
    r.headers_.assign(hdr.data(), hdr.size());

    // Body from pool
    r.body_pool_ = pool.Dup(body);

    return r;
}

// ── Body accessor ──

std::string_view Response::Body() const
{
    if (pool_)
        return body_pool_;
    return body_own_;
}

// ── AddHeader ──

void Response::AddHeader(const std::string& line)
{
    auto pos = headers_.find("\r\n\r\n");
    if (pos != std::string::npos) {
        headers_.insert(pos + 2, line + "\r\n");
    }
}
