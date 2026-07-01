#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>

// ═══════════════════════════════════════════════════════════════
// BFL — Binary Frame Layer
//
// Pure functions for HTTP/2 frame encoding and decoding.
// No I/O, no state, no allocation — operates on raw buffers.
// ═══════════════════════════════════════════════════════════════

// ── Constants ──
inline constexpr std::string_view kH2ClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr size_t           kH2PrefaceLen    = 24;
inline constexpr size_t           kFrameHeaderSize = 9;
inline constexpr uint32_t         kDefaultMaxFrameSize  = 16384;
inline constexpr uint32_t         kDefaultWindowSize    = 65535;
inline constexpr uint32_t         kDefaultHeaderTableSz = 4096;

// ── Frame type (1 byte on wire) ──
enum class H2FrameType : uint8_t {
    DATA          = 0x00,
    HEADERS       = 0x01,
    PRIORITY      = 0x02,
    RST_STREAM    = 0x03,
    SETTINGS      = 0x04,
    PUSH_PROMISE  = 0x05,
    PING          = 0x06,
    GOAWAY        = 0x07,
    WINDOW_UPDATE = 0x08,
    CONTINUATION  = 0x09,
};

// ── Error codes ──
enum class H2Error : uint32_t {
    NO_ERROR            = 0x00,
    PROTOCOL_ERROR      = 0x01,
    INTERNAL_ERROR      = 0x02,
    FLOW_CONTROL_ERROR  = 0x03,
    SETTINGS_TIMEOUT    = 0x04,
    STREAM_CLOSED       = 0x05,
    FRAME_SIZE_ERROR    = 0x06,
    REFUSED_STREAM      = 0x07,
    CANCEL              = 0x08,
    COMPRESSION_ERROR   = 0x09,
    CONNECT_ERROR       = 0x0A,
    ENHANCE_YOUR_CALM   = 0x0B,
    INADEQUATE_SECURITY = 0x0C,
    HTTP_1_1_REQUIRED   = 0x0D,
};

// ── Frame flags ──
namespace H2Flags {
    inline constexpr uint8_t END_STREAM  = 0x01;
    inline constexpr uint8_t ACK         = 0x01;  // SETTINGS / PING
    inline constexpr uint8_t END_HEADERS = 0x04;
    inline constexpr uint8_t PADDED      = 0x08;
    inline constexpr uint8_t PRIORITY    = 0x20;  // HEADERS only
}

// ═══════════════════════════════════════════════════════════════
// Frame header (9 bytes on wire)
// ═══════════════════════════════════════════════════════════════

struct H2FrameHeader {
    uint32_t    length;     // 24-bit big-endian (payload only, excl. 9-byte header)
    H2FrameType type;
    uint8_t     flags;
    uint32_t    stream_id;  // 31-bit big-endian
};

/// Decode a 9-byte frame header from wire format.
/// @pre  `data` points to at least 9 readable bytes.
inline H2FrameHeader DecodeFrameHeader(const uint8_t* data) {
    return {
        .length    = (static_cast<uint32_t>(data[0]) << 16)
                   | (static_cast<uint32_t>(data[1]) <<  8)
                   |  static_cast<uint32_t>(data[2]),
        .type      = static_cast<H2FrameType>(data[3]),
        .flags     = data[4],
        .stream_id = (static_cast<uint32_t>(data[5] & 0x7f) << 24)
                   | (static_cast<uint32_t>(data[6])        << 16)
                   | (static_cast<uint32_t>(data[7])        <<  8)
                   |  static_cast<uint32_t>(data[8]),
    };
}

/// Encode a frame header to wire format.
/// @param[out] dst  Must point to 9 writable bytes.
inline void EncodeFrameHeader(uint8_t* dst, const H2FrameHeader& hdr) {
    dst[0] = static_cast<uint8_t>(hdr.length >> 16);
    dst[1] = static_cast<uint8_t>(hdr.length >>  8);
    dst[2] = static_cast<uint8_t>(hdr.length);
    dst[3] = static_cast<uint8_t>(hdr.type);
    dst[4] = hdr.flags;
    dst[5] = static_cast<uint8_t>((hdr.stream_id >> 24) & 0x7f);
    dst[6] = static_cast<uint8_t>(hdr.stream_id >> 16);
    dst[7] = static_cast<uint8_t>(hdr.stream_id >>  8);
    dst[8] = static_cast<uint8_t>(hdr.stream_id);
}

// ═══════════════════════════════════════════════════════════════
// PRIORITY frame / HEADERS priority payload (5 bytes)
// ═══════════════════════════════════════════════════════════════

struct H2PriorityPayload {
    bool     exclusive;
    uint32_t stream_dependency;  // 31-bit
    uint8_t  weight;             // 1–256 (wire = value - 1)
};

/// @pre  `data` points to 5 payload bytes.
inline H2PriorityPayload DecodePriority(const uint8_t* data) {
    return {
        .exclusive         = (data[0] & 0x80) != 0,
        .stream_dependency = (static_cast<uint32_t>(data[0] & 0x7f) << 24)
                           | (static_cast<uint32_t>(data[1])        << 16)
                           | (static_cast<uint32_t>(data[2])        <<  8)
                           |  static_cast<uint32_t>(data[3]),
        .weight = static_cast<uint8_t>(data[4] + 1),
    };
}

inline void EncodePriority(uint8_t* dst, const H2PriorityPayload& p) {
    uint32_t sd = p.stream_dependency & 0x7fffffff;
    if (p.exclusive) sd |= 0x80000000u;
    dst[0] = static_cast<uint8_t>(sd >> 24);
    dst[1] = static_cast<uint8_t>(sd >> 16);
    dst[2] = static_cast<uint8_t>(sd >>  8);
    dst[3] = static_cast<uint8_t>(sd);
    dst[4] = static_cast<uint8_t>(p.weight - 1);
}

// ═══════════════════════════════════════════════════════════════
// RST_STREAM (4 bytes)
// ═══════════════════════════════════════════════════════════════

struct H2RstStream {
    H2Error error_code;
};

inline H2RstStream DecodeRstStream(const uint8_t* data) {
    uint32_t ec = (static_cast<uint32_t>(data[0]) << 24)
                | (static_cast<uint32_t>(data[1]) << 16)
                | (static_cast<uint32_t>(data[2]) <<  8)
                |  static_cast<uint32_t>(data[3]);
    return {static_cast<H2Error>(ec)};
}

inline void EncodeRstStream(uint8_t* dst, H2Error ec) {
    uint32_t v = static_cast<uint32_t>(ec);
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>(v >> 16);
    dst[2] = static_cast<uint8_t>(v >>  8);
    dst[3] = static_cast<uint8_t>(v);
}

// ═══════════════════════════════════════════════════════════════
// SETTINGS (each entry = 6 bytes: 2-byte id + 4-byte value)
// ═══════════════════════════════════════════════════════════════

struct H2Settings {
    std::optional<uint32_t> header_table_size;
    std::optional<uint32_t> enable_push;
    std::optional<uint32_t> max_concurrent_streams;
    std::optional<uint32_t> initial_window_size;
    std::optional<uint32_t> max_frame_size;
    std::optional<uint32_t> max_header_list_size;
    std::optional<uint32_t> enable_connect_protocol;  // 0x08, RFC 8441
};

inline H2Settings DecodeSettings(const uint8_t* data, size_t len) {
    H2Settings s;
    for (size_t i = 0; i + 6 <= len; i += 6) {
        uint16_t id = (static_cast<uint16_t>(data[i]) << 8) | data[i + 1];
        uint32_t v  = (static_cast<uint32_t>(data[i+2]) << 24)
                    | (static_cast<uint32_t>(data[i+3]) << 16)
                    | (static_cast<uint32_t>(data[i+4]) <<  8)
                    |  static_cast<uint32_t>(data[i+5]);
        switch (id) {
        case 0x01: s.header_table_size      = v; break;
        case 0x02: s.enable_push            = v; break;
        case 0x03: s.max_concurrent_streams = v; break;
        case 0x04: s.initial_window_size    = v; break;
        case 0x05: s.max_frame_size         = v; break;
        case 0x06: s.max_header_list_size   = v; break;
        case 0x08: s.enable_connect_protocol = v; break;
        }
    }
    return s;
}

/// Returns 6 × number of set entries.
inline size_t EncodeSettings(uint8_t* dst, const H2Settings& s) {
    size_t n = 0;
    auto emit = [&](uint16_t id, uint32_t v) {
        dst[0] = static_cast<uint8_t>(id >> 8);
        dst[1] = static_cast<uint8_t>(id);
        dst[2] = static_cast<uint8_t>(v >> 24);
        dst[3] = static_cast<uint8_t>(v >> 16);
        dst[4] = static_cast<uint8_t>(v >>  8);
        dst[5] = static_cast<uint8_t>(v);
        dst += 6; ++n;
    };
    if (s.header_table_size)      emit(0x01, *s.header_table_size);
    if (s.enable_push)            emit(0x02, *s.enable_push);
    if (s.max_concurrent_streams) emit(0x03, *s.max_concurrent_streams);
    if (s.initial_window_size)    emit(0x04, *s.initial_window_size);
    if (s.max_frame_size)         emit(0x05, *s.max_frame_size);
    if (s.max_header_list_size)   emit(0x06, *s.max_header_list_size);
    if (s.enable_connect_protocol) emit(0x08, *s.enable_connect_protocol);
    return n * 6;
}

// ═══════════════════════════════════════════════════════════════
// PUSH_PROMISE (4-byte promised stream id + HPACK block)
// ═══════════════════════════════════════════════════════════════

struct H2PushPromise {
    uint32_t promised_stream_id;  // 31-bit
};

inline H2PushPromise DecodePushPromise(const uint8_t* data) {
    return {
        .promised_stream_id = (static_cast<uint32_t>(data[0] & 0x7f) << 24)
                            | (static_cast<uint32_t>(data[1])        << 16)
                            | (static_cast<uint32_t>(data[2])        <<  8)
                            |  static_cast<uint32_t>(data[3])
    };
}

inline void EncodePushPromise(uint8_t* dst, uint32_t promised_id) {
    dst[0] = static_cast<uint8_t>((promised_id >> 24) & 0x7f);
    dst[1] = static_cast<uint8_t>(promised_id >> 16);
    dst[2] = static_cast<uint8_t>(promised_id >>  8);
    dst[3] = static_cast<uint8_t>(promised_id);
}

// ═══════════════════════════════════════════════════════════════
// PING (8 opaque bytes)
// ═══════════════════════════════════════════════════════════════

struct H2Ping {
    uint8_t opaque_data[8];
};

inline H2Ping DecodePing(const uint8_t* data) {
    H2Ping p;
    std::memcpy(p.opaque_data, data, 8);
    return p;
}

inline void EncodePing(uint8_t* dst, const H2Ping& p) {
    std::memcpy(dst, p.opaque_data, 8);
}

// ═══════════════════════════════════════════════════════════════
// GOAWAY (8 bytes fixed + optional debug data)
// ═══════════════════════════════════════════════════════════════

struct H2GoAway {
    uint32_t last_stream_id;  // 31-bit
    H2Error  error_code;
};

inline H2GoAway DecodeGoAway(const uint8_t* data) {
    return {
        .last_stream_id = (static_cast<uint32_t>(data[0] & 0x7f) << 24)
                        | (static_cast<uint32_t>(data[1])        << 16)
                        | (static_cast<uint32_t>(data[2])        <<  8)
                        |  static_cast<uint32_t>(data[3]),
        .error_code     = static_cast<H2Error>(
                            (static_cast<uint32_t>(data[4]) << 24)
                          | (static_cast<uint32_t>(data[5]) << 16)
                          | (static_cast<uint32_t>(data[6]) <<  8)
                          |  static_cast<uint32_t>(data[7])),
    };
}

inline void EncodeGoAway(uint8_t* dst, const H2GoAway& g) {
    dst[0] = static_cast<uint8_t>((g.last_stream_id >> 24) & 0x7f);
    dst[1] = static_cast<uint8_t>(g.last_stream_id >> 16);
    dst[2] = static_cast<uint8_t>(g.last_stream_id >>  8);
    dst[3] = static_cast<uint8_t>(g.last_stream_id);
    uint32_t ec = static_cast<uint32_t>(g.error_code);
    dst[4] = static_cast<uint8_t>(ec >> 24);
    dst[5] = static_cast<uint8_t>(ec >> 16);
    dst[6] = static_cast<uint8_t>(ec >>  8);
    dst[7] = static_cast<uint8_t>(ec);
}

// ═══════════════════════════════════════════════════════════════
// WINDOW_UPDATE (4 bytes, 31-bit increment)
// ═══════════════════════════════════════════════════════════════

struct H2WindowUpdate {
    uint32_t increment;  // 31-bit, valid range 1–2147483647
};

inline H2WindowUpdate DecodeWindowUpdate(const uint8_t* data) {
    return {
        .increment = (static_cast<uint32_t>(data[0] & 0x7f) << 24)
                   | (static_cast<uint32_t>(data[1])        << 16)
                   | (static_cast<uint32_t>(data[2])        <<  8)
                   |  static_cast<uint32_t>(data[3])
    };
}

inline void EncodeWindowUpdate(uint8_t* dst, uint32_t increment) {
    dst[0] = static_cast<uint8_t>((increment >> 24) & 0x7f);
    dst[1] = static_cast<uint8_t>(increment >> 16);
    dst[2] = static_cast<uint8_t>(increment >>  8);
    dst[3] = static_cast<uint8_t>(increment);
}

// ═══════════════════════════════════════════════════════════════
// HEADERS / DATA frame helpers (padding + priority)
// ═══════════════════════════════════════════════════════════════

/// Offset of the HPACK block within a HEADERS payload.
inline size_t HeadersBlockStart(const H2FrameHeader& hdr) {
    size_t start = 0;
    if (hdr.flags & H2Flags::PADDED)  start += 1;  // Pad Length byte
    if (hdr.flags & H2Flags::PRIORITY) start += 5;  // priority fields
    return start;
}

/// Length of the HPACK block within a HEADERS payload (excluding trailing padding).
inline size_t HeadersBlockLength(const H2FrameHeader& hdr, const uint8_t* payload) {
    size_t overhead = !!(hdr.flags & H2Flags::PADDED)
                    + 5 * !!(hdr.flags & H2Flags::PRIORITY);
    if (hdr.flags & H2Flags::PADDED) {
        uint8_t pad_len = payload[0];
        size_t raw = hdr.length - overhead;
        return (pad_len <= raw) ? raw - pad_len : 0;
    }
    return hdr.length - overhead;
}

/// Offset of data within a DATA frame payload.
inline size_t DataOffset(const H2FrameHeader& hdr) {
    return (hdr.flags & H2Flags::PADDED) ? 1 : 0;
}

/// Actual data length in a DATA frame (excluding optional padding).
inline size_t DataLength(const H2FrameHeader& hdr, const uint8_t* payload) {
    if (hdr.flags & H2Flags::PADDED) {
        uint8_t pad_len = payload[0];
        size_t raw = hdr.length - 1;
        return (pad_len <= raw) ? raw - pad_len : 0;
    }
    return hdr.length;
}
