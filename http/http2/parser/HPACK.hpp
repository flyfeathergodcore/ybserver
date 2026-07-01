#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <string>
#include <vector>
#include <deque>
#include <utility>

class H2StreamContext;
class SessionRegion;

// ═══════════════════════════════════════════════════════════════
// HPACK — RFC 7541 Header Compression
//
// Pure functions for HTTP/2 header compression encoding and decoding.
// ──
// HpackDecoder  — Decode HPACK blocks from incoming HEADERS frames.
//                 Manages the dynamic table and handles Huffman decoding.
// HpackEncoder  — Encode response headers into HPACK blocks.
//                 Uses static table indexing + literal never-indexed.
// ═══════════════════════════════════════════════════════════════

class HpackDecoder {
public:
    HpackDecoder() = default;

    /// Set max dynamic table size (from peer SETTINGS HEADER_TABLE_SIZE).
    void SetMaxTableSize(uint32_t size);

    /// Decode a HPACK-encoded header block.
    /// Calls ctx.AddHeader() for each decoded header.
    /// Returns true on success, false on protocol error.
    bool Decode(const uint8_t* data, size_t len, H2StreamContext& ctx);

private:
    // ── Static/dynamic table data ──
    struct HeaderField { std::string name; std::string value; };

    static std::string_view StaticName(int index);   // 1-based
    static std::string_view StaticValue(int index);  // 1-based

    std::deque<HeaderField> dynamic_table_;
    uint32_t max_table_size_   = 4096;
    uint32_t current_table_size_ = 0;

    // ── Decode helpers ──
    uint32_t DecodeInteger(const uint8_t*& data, size_t& len, uint8_t prefix_bits);
    std::string_view DecodeString(const uint8_t*& data, size_t& len,
                                   SessionRegion& region);
    std::string HuffmanDecode(const uint8_t* data, size_t len);

    // ── Dynamic table helpers ──
    static size_t EntryOverhead(std::string_view n, std::string_view v);
    void EvictTo(uint32_t target_size);

    // ── Name/value lookup by combined index ──
    std::pair<std::string_view, std::string_view> Lookup(int index) const;
};


class HpackEncoder {
public:
    HpackEncoder() = default;

    /// Encode response headers into HPACK format.
    /// Uses static table indexing for common headers,
    /// literal never-indexed for everything else.
    std::vector<uint8_t> Encode(
        const std::vector<std::pair<std::string_view, std::string_view>>& headers);

private:
    void EncodeInteger(std::vector<uint8_t>& out, uint32_t value, uint8_t prefix_bits);
    void EncodeString(std::vector<uint8_t>& out, std::string_view str);

    /// Look up (name, value) in static table. Returns 1-based index or 0.
    int LookupStatic(std::string_view name, std::string_view value) const;
    int LookupStaticNameOnly(std::string_view name) const;
};
