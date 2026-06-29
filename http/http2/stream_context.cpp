#include "http/http2/stream_context.hpp"
#include <cstring>

void H2StreamContext::SetMethod(std::string_view m)
{
    auto* pool = Pool();
    if (pool) method_ = pool->DupOff(m);
}

void H2StreamContext::SetPath(std::string_view p)
{
    auto* pool = Pool();
    if (pool) path_ = pool->DupOff(p);
}

void H2StreamContext::AddHeader(std::string_view name, std::string_view value)
{
    if (header_count_ >= kMaxHeaders) return;
    auto* pool = Pool();
    if (!pool) return;
    headers_[header_count_].name  = pool->DupOff(name);
    headers_[header_count_].value = pool->DupOff(value);
    header_count_++;
}

void H2StreamContext::AppendBody(const uint8_t* data, size_t len)
{
    if (len == 0) return;
    auto* pool = Pool();
    if (!pool) return;
    void* buf = pool->Alloc(len);
    if (!buf) return;
    std::memcpy(buf, data, len);
    body_ = {static_cast<uint32_t>(static_cast<char*>(buf) - pool->Data()),
             static_cast<uint32_t>(len)};
}

std::string_view H2StreamContext::Method() const
{
    auto* r = Pool();
    return r ? r->ToView(method_) : std::string_view{};
}

std::string_view H2StreamContext::Path() const
{
    auto* r = Pool();
    return r ? r->ToView(path_) : std::string_view{};
}

std::string_view H2StreamContext::Body() const
{
    auto* r = Pool();
    return r ? r->ToView(body_) : std::string_view{};
}

std::string_view H2StreamContext::Header(std::string_view key) const
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
