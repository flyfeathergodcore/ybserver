#pragma once
#include "net/session_base.hpp"
#include "http/http2/stream_context.hpp"
#include "http/http2/ws_connection.hpp"
#include "http/http2/parser/BFL.hpp"
#include "http/http2/parser/HPACK.hpp"
#include "http/http2/parser/stream_manager.hpp"
#include "http/http2/parser/flow_control.hpp"
#include <asio/ssl.hpp>
#include <unordered_map>
#include <vector>
#include <array>
#include <memory>

class RegionPool;

using asio::ip::tcp;

// ── H2Session ──
//
// HTTP/2 session over TLS (h2).  Uses our custom H2 stack:
//   BFL          — frame encoding/decoding
//   HPACK        — header compression
//   StreamManager — stream lifecycle + pending queue
//   FlowControl  — connection/stream window management
//
// Stream handling is SEQUENTIAL for normal HTTP requests: when a
// complete request is received, the stream ID is added to a pending
// queue and the main loop drains it one at a time via HandleStream.
//
// WebSocket (RFC 8441 Extended CONNECT) streams are handled
// concurrently: HandleStream spawns the WS handler via co_spawn and
// the main loop continues processing other streams + reading data.
//

class H2Session : public SessionBase {
public:
    H2Session(asio::ssl::stream<tcp::socket> stream,
              Router& router,
              MiddlewareManager& middleware,
              RegionPool* region_pool);
    ~H2Session() override;

    asio::awaitable<void> Start() override;

private:
    // ── Core ──
    asio::ssl::stream<tcp::socket> stream_;
    asio::any_io_executor exec_;

    // ── Custom H2 modules ──
    HpackDecoder    hpack_decoder_;
    HpackEncoder    hpack_encoder_;
    H2StreamManager stream_mgr_;
    H2FlowControl   flow_control_;

    // ── Per-stream HTTP context ──
    std::unordered_map<int32_t, H2StreamContext> streams_;

    // ── I/O buffers ──
    static constexpr size_t kReadBufSize = 65536;
    std::array<uint8_t, kReadBufSize> read_buf_;
    size_t read_buf_used_ = 0;
    std::vector<uint8_t> output_;
    bool writing_ = false;

    // ── Local settings (advertised to peer) ──
    H2Settings local_settings_;

    // ── Peer settings (from peer's SETTINGS) ──
    uint32_t peer_max_concurrent_ = 0;     // 0 = unlimited
    uint32_t peer_initial_window_ = 65535;
    uint32_t peer_max_frame_size_  = 16384;

    // ── State ──
    bool goaway_received_ = false;
    bool goaway_sent_ = false;

    // ── CONTINUATION reassembly ──
    int32_t  continuation_stream_id_ = 0;
    std::vector<uint8_t> continuation_block_;

    // ── Frame dispatch ──
    void ProcessFrame(const H2FrameHeader& hdr, const uint8_t* payload);

    void OnSettings(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnHeaders(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnData(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnRstStream(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnPing(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnGoAway(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnWindowUpdate(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnPriority(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnContinuation(const H2FrameHeader& hdr, const uint8_t* payload);

    // ── Output helpers ──
    void WriteHeaders(int32_t sid, const std::vector<uint8_t>& hpack, bool end_headers);
    void WriteData(int32_t sid, const uint8_t* data, size_t len, bool end_stream);
    void WriteRstStream(int32_t sid, H2Error err);
    void WriteGoAway(int32_t last_sid, H2Error err);
    void WriteWindowUpdate(int32_t sid, uint32_t increment);
    void WritePingAck(const H2Ping& ping);
    void WriteSettingsAck();

    /// Encode response headers and write HEADERS frame.
    void WriteResponseHeaders(int32_t sid, const Response& resp);

    // ── I/O ──
    asio::awaitable<bool> FlushOutput();
    asio::awaitable<void> ProcessPending();

    // ── Stream handler ──
    asio::awaitable<void> HandleStream(int32_t stream_id);
};
