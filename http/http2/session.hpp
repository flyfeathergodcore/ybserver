#pragma once
#include "net/session_base.hpp"
#include "http/http2/stream_context.hpp"
#include <asio/ssl.hpp>
#include <nghttp2/nghttp2.h>
#include <unordered_map>
#include <vector>
#include <deque>

class RegionPool;

using asio::ip::tcp;

// ── H2Session ──
//
// HTTP/2 session over TLS (h2).  Uses nghttp2 for framing, HPACK,
// flow control, and stream management.
//
// Stream handling is SEQUENTIAL: when a complete request is received
// (HEADERS + END_STREAM, or DATA + END_STREAM for POST bodies), the
// stream ID is added to a pending queue.  The main event loop drains
// this queue one at a time via co_await HandleStream, avoiding all
// concurrency races (header callback overwrite, FlushOutput reentrancy)
// that would arise from co_spawn-based concurrent stream processing.
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
    // ── nghttp2 C callbacks → instance dispatchers ──
    static int cb_on_begin_headers(nghttp2_session*, const nghttp2_frame*, void*);
    static int cb_on_header(nghttp2_session*, const nghttp2_frame*,
                            const uint8_t*, size_t, const uint8_t*, size_t,
                            uint8_t, void*);
    static int cb_on_frame_recv(nghttp2_session*, const nghttp2_frame*, void*);
    static int cb_on_stream_close(nghttp2_session*, int32_t, uint32_t, void*);
    static int cb_on_data_chunk_recv(nghttp2_session*, uint8_t, int32_t,
                                      const uint8_t*, size_t, void*);
    static ssize_t cb_send(nghttp2_session*, const uint8_t*, size_t, int, void*);
    static ssize_t cb_data_read(nghttp2_session*, int32_t, uint8_t*,
                                 size_t, uint32_t*, nghttp2_data_source*, void*);

    // ── Instance handlers (called from static callbacks) ──
    void OnBeginHeaders(int32_t stream_id);
    void OnHeader(int32_t stream_id, std::string_view name, std::string_view value);
    void OnFrameRecv(const nghttp2_frame* frame);
    void OnStreamClose(int32_t stream_id);
    void OnDataChunk(int32_t stream_id, const uint8_t* data, size_t len);

    asio::awaitable<void> HandleStream(int32_t stream_id);

    // ── I/O ──
    asio::awaitable<bool> FlushOutput();

    // ── Helpers ──
    static nghttp2_session_callbacks* GetCallbacks();

    // ── Members ──
    asio::ssl::stream<tcp::socket> stream_;
    nghttp2_session* session_ = nullptr;
    asio::any_io_executor exec_;

    // Per-stream contexts (stream_id → context)
    std::unordered_map<int32_t, H2StreamContext> streams_;

    // Streams whose full request has been received and await handling.
    // Processed sequentially in the main loop (avoids concurrency races).
    std::deque<int32_t> pending_;

    // Output buffer (filled by cb_send, drained by FlushOutput)
    std::vector<uint8_t> output_;
    bool writing_ = false;

    // ── Per-connection reusable resources ──

    // Reusable nv vector (avoids per-request heap alloc).
    std::vector<nghttp2_nv> nv_reuse_;

    // Greedy read buffer (64KB — reads accumulate here per wakeup cycle).
    static constexpr size_t kReadBufSize = 65536;
    std::array<uint8_t, kReadBufSize> read_buf_;
    size_t read_buf_used_ = 0;
};
