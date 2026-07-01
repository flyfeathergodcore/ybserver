#pragma once
#include <asio.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
// WebSocket frame types (RFC 6455)
// ═══════════════════════════════════════════════════════════════════

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct WsFrame {
    bool    fin     = true;
    WsOpcode opcode = WsOpcode::Binary;
    std::string payload;   // unmasked payload data
};

// ── Frame I/O (template, inline) ──

/// Read exactly n bytes from stream (async).
template<typename Stream>
asio::awaitable<std::string> ReadExactly(Stream& stream, size_t n)
{
    std::string buf(n, '\0');
    if (n == 0) co_return buf;

    size_t total = 0;
    while (total < n)
    {
        auto [ec, bytes] = co_await stream.async_read_some(
            asio::buffer(buf.data() + total, n - total),
            asio::as_tuple(asio::use_awaitable));
        if (ec) co_return std::string();  // connection error
        total += bytes;
    }
    co_return buf;
}

/// Unmask payload in-place (XOR with 4-byte masking key).
inline void UnmaskPayload(std::string& payload, const uint8_t mask_key[4])
{
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] ^= mask_key[i & 3];
}

/// Read one WebSocket frame from stream.
/// Returns empty frame (opcode=Binary, fin=true, payload empty) on error/close.
template<typename Stream>
asio::awaitable<WsFrame> ReadFrame(Stream& stream)
{
    WsFrame frame;

    // 1. Read 2-byte header
    auto hdr = co_await ReadExactly(stream, 2);
    if (hdr.size() < 2) co_return frame;
    auto h = reinterpret_cast<const uint8_t*>(hdr.data());

    frame.fin     = (h[0] & 0x80) != 0;
    // RSV bits ignored
    frame.opcode  = static_cast<WsOpcode>(h[0] & 0x0F);
    bool masked   = (h[1] & 0x80) != 0;
    uint64_t len  = h[1] & 0x7F;

    // 2. Extended payload length
    if (len == 126) {
        auto ext = co_await ReadExactly(stream, 2);
        if (ext.size() < 2) co_return frame;
        len = (static_cast<uint64_t>(static_cast<uint8_t>(ext[0])) << 8)
            | static_cast<uint64_t>(static_cast<uint8_t>(ext[1]));
    } else if (len == 127) {
        auto ext = co_await ReadExactly(stream, 8);
        if (ext.size() < 8) co_return frame;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | static_cast<uint64_t>(static_cast<uint8_t>(ext[i]));
    }

    // 3. Masking key (client→server only)
    uint8_t mask_key[4] = {};
    if (masked) {
        auto mk = co_await ReadExactly(stream, 4);
        if (mk.size() < 4) co_return frame;
        std::memcpy(mask_key, mk.data(), 4);
    }

    // 4. Payload — limit to 1 MB for safety
    constexpr uint64_t kMaxPayload = 1024 * 1024;
    if (len > kMaxPayload) co_return frame;
    if (len > 0) {
        frame.payload = co_await ReadExactly(stream, static_cast<size_t>(len));
        if (frame.payload.size() < static_cast<size_t>(len))
            co_return WsFrame{};  // read error
        if (masked)
            UnmaskPayload(frame.payload, mask_key);
    }

    co_return frame;
}

/// Write one WebSocket frame to stream.
/// Server → Client: mask=false.  Client → Server: mask=true (relay).
template<typename Stream>
asio::awaitable<void> WriteFrame(Stream& stream, WsOpcode opcode,
                                  std::string payload, bool fin = true,
                                  bool mask = false)
{
    std::string header;
    header.reserve(14 + payload.size());

    // Byte 0: FIN + opcode
    uint8_t b0 = (fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode);
    header += static_cast<char>(b0);

    // Byte 1: MASK + payload_len
    uint64_t len = payload.size();
    uint8_t b1 = (mask ? 0x80 : 0x00);
    if (len < 126) {
        b1 |= static_cast<uint8_t>(len);
        header += static_cast<char>(b1);
    } else if (len <= 0xFFFF) {
        b1 |= 126;
        header += static_cast<char>(b1);
        header += static_cast<char>((len >> 8) & 0xFF);
        header += static_cast<char>(len & 0xFF);
    } else {
        b1 |= 127;
        header += static_cast<char>(b1);
        for (int i = 7; i >= 0; i--)
            header += static_cast<char>((len >> (i * 8)) & 0xFF);
    }

    // Masking key + masked payload
    if (mask) {
        // Use a fixed key for now (non-cryptographic, just protocol compliant)
        uint8_t mk[4] = {0x00, 0x00, 0x00, 0x00};
        header.append(reinterpret_cast<const char*>(mk), 4);
        for (size_t i = 0; i < payload.size(); i++)
            payload[i] ^= mk[i & 3];
    }

    header += payload;
    co_await async_write(stream, asio::buffer(header),
                         asio::use_awaitable);
}

/// Convenience: send a Close frame.
template<typename Stream>
asio::awaitable<void> WriteCloseFrame(Stream& stream,
                                       uint16_t code = 1000,
                                       std::string_view reason = {})
{
    std::string payload;
    payload += static_cast<char>((code >> 8) & 0xFF);
    payload += static_cast<char>(code & 0xFF);
    payload += reason;
    co_await WriteFrame(stream, WsOpcode::Close, std::move(payload));
}

/// Compute Sec-WebSocket-Accept value from client key.
std::string ComputeWsAccept(std::string_view client_key);
