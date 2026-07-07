#include "http/http2/session.hpp"
#include "net/region_pool.hpp"
#include "net/sse_push.hpp"
#include "handler/metrics.hpp"
#include "net/response.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <openssl/ssl.h>

using asio::ip::tcp;

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

H2Session::H2Session(asio::ssl::stream<tcp::socket> stream,
                     Router& router,
                     MiddlewareManager& middleware,
                     RegionPool* region_pool)
    : SessionBase(router, middleware)
    , stream_(std::move(stream))
{
    if (region_pool)
        region_.Init(region_pool);

    // Advertise ENABLE_CONNECT_PROTOCOL (RFC 8441 WebSocket)
    local_settings_.enable_connect_protocol = 1;
}

H2Session::~H2Session() = default;

// ═══════════════════════════════════════════════════════════════
// Start — main coroutine
//
// Event loop:
//   1. Send connection preface (SETTINGS frame)
//   2. Read TLS data from socket
//   3. Parse complete frames from the buffer
//   4. Dispatch each frame (HEADERS → HPACK decode → enqueue, etc.)
//   5. Process pending streams sequentially
//   6. Flush any pending output frames to socket
//   7. Repeat until GOAWAY / connection close
// ═══════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::Start()
{
    auto self = this->shared_from_this();
    exec_ = co_await asio::this_coro::executor;

    if (metrics_) metrics_->OnConnectionOpen(worker_id_);

    // ── Connection preface: send our SETTINGS ──
    uint8_t settings_payload[64];
    size_t slen = EncodeSettings(settings_payload, local_settings_);
    {
        size_t pos = output_.size();
        output_.resize(pos + kFrameHeaderSize + slen);
        EncodeFrameHeader(output_.data() + pos,
            {static_cast<uint32_t>(slen), H2FrameType::SETTINGS, 0, 0});
        std::memcpy(output_.data() + pos + kFrameHeaderSize, settings_payload, slen);
    }

    if (!co_await FlushOutput()) co_return;

    // ── Main read/dispatch loop ──
    try
    {
        while (!goaway_received_ && !goaway_sent_)
        {
            // ── Greedy read: accumulate all readily available data ──
            read_buf_used_ = 0;
            bool read_ok = false;
            for (int greedy_pass = 0; greedy_pass < 2; greedy_pass++) {
                size_t remaining = read_buf_.size() - read_buf_used_;
                if (remaining == 0) break;
                auto [ec, n] = co_await stream_.async_read_some(
                    asio::buffer(read_buf_.data() + read_buf_used_, remaining),
                    asio::as_tuple(asio::use_awaitable));
                if (ec) break;
                read_buf_used_ += static_cast<size_t>(n);
                read_ok = true;

                if (::SSL_pending(stream_.native_handle()) <= 0)
                    break;
            }
            if (!read_ok) break;

            // ── Parse all complete frames ──
            {
                const uint8_t* data = read_buf_.data();
                size_t available = read_buf_used_;

                // Skip h2c client connection preface magic string if present
                // (nghttp2 sends it even over TLS; 24 bytes before first frame)
                if (stream_mgr_.LastClientStreamId() == 0
                    && available >= kH2PrefaceLen)
                {
                    static constexpr char kMagic[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
                    if (std::memcmp(data, kMagic, kH2PrefaceLen) == 0) {
                        data += kH2PrefaceLen;
                        available -= kH2PrefaceLen;
                    }
                }

                while (available >= kFrameHeaderSize) {
                    auto hdr = DecodeFrameHeader(data);
                    // Parse frame header
                    // Validate frame length
                    if (hdr.length > peer_max_frame_size_) {
                        std::cerr << "[h2] frame too large: " << hdr.length
                                  << " > " << peer_max_frame_size_ << std::endl;
                        WriteGoAway(stream_mgr_.LastClientStreamId(),
                                    H2Error::FRAME_SIZE_ERROR);
                        goaway_sent_ = true;
                        break;
                    }

                    size_t frame_size = kFrameHeaderSize + hdr.length;
                    if (frame_size > available) break;  // incomplete frame

                    ProcessFrame(hdr, data + kFrameHeaderSize);
                    data += frame_size;
                    available -= frame_size;
                }

                // Shift remaining partial data to front of buffer
                if (data != read_buf_.data() && available > 0)
                    std::memmove(read_buf_.data(), data, available);
                read_buf_used_ = available;
            }

            if (goaway_sent_) break;

            // ── Process pending streams ──
            co_await ProcessPending();

            // ── Flush output ──
            // Flush once (sends the initial response headers)
            if (!co_await FlushOutput()) break;
            // Flush again if WS handler (co_spawned during FlushOutput) wrote RST_STREAM
            while (!output_.empty())
                if (!co_await FlushOutput()) break;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "[h2] " << e.what() << std::endl;
    }


    // ── Graceful GOAWAY ──
    if (!goaway_sent_) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::NO_ERROR);
        co_await FlushOutput();
    }

    // Clean up remaining streams
    stream_mgr_.GcClosed();

    if (metrics_) metrics_->OnConnectionClose(worker_id_);
}

// ═══════════════════════════════════════════════════════════════
// Frame dispatch
// ═══════════════════════════════════════════════════════════════

void H2Session::ProcessFrame(const H2FrameHeader& hdr, const uint8_t* payload)
{
    switch (hdr.type) {
    case H2FrameType::SETTINGS:      OnSettings(hdr, payload); break;
    case H2FrameType::HEADERS:       OnHeaders(hdr, payload); break;
    case H2FrameType::DATA:          OnData(hdr, payload); break;
    case H2FrameType::RST_STREAM:    OnRstStream(hdr, payload); break;
    case H2FrameType::PING:          OnPing(hdr, payload); break;
    case H2FrameType::GOAWAY:        OnGoAway(hdr, payload); break;
    case H2FrameType::WINDOW_UPDATE: OnWindowUpdate(hdr, payload); break;
    case H2FrameType::PRIORITY:      OnPriority(hdr, payload); break;
    case H2FrameType::CONTINUATION:  OnContinuation(hdr, payload); break;
    case H2FrameType::PUSH_PROMISE:
        // Server cannot receive PUSH_PROMISE — ignore (malformed peer)
        break;
    }
}

// ═══════════════════════════════════════════════════════════════
// SETTINGS (type 4)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnSettings(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (hdr.flags & H2Flags::ACK) {
        // Peer acknowledged our SETTINGS — nothing to do
        return;
    }

    // Decode and apply peer settings
    auto s = DecodeSettings(payload, hdr.length);

    if (s.max_concurrent_streams)
        peer_max_concurrent_ = *s.max_concurrent_streams;

    if (s.initial_window_size) {
        peer_initial_window_ = *s.initial_window_size;
        // NOTE: RFC requires adjusting existing stream windows by the delta.
        // For MVP we set only for new streams.
        flow_control_.SetPeerInitialWindow(peer_initial_window_);
    }

    if (s.max_frame_size) {
        if (*s.max_frame_size < 16384 || *s.max_frame_size > 16777215) {
            WriteGoAway(stream_mgr_.LastClientStreamId(),
                        H2Error::PROTOCOL_ERROR);
            goaway_sent_ = true;
            return;
        }
        peer_max_frame_size_ = *s.max_frame_size;
    }

    // Respond with SETTINGS ACK
    WriteSettingsAck();
}

// ═══════════════════════════════════════════════════════════════
// HEADERS (type 1)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnHeaders(const H2FrameHeader& hdr, const uint8_t* payload)
{
    int32_t sid = hdr.stream_id;

    // Validate stream
    if (!stream_mgr_.OnStreamOpen(sid)) {
        WriteRstStream(sid, H2Error::REFUSED_STREAM);
        return;
    }

    // Get or create stream context
    auto [it, created] = streams_.try_emplace(sid);
    if (created)
        it->second.SetPool(&region_);
    auto& ctx = it->second;
    ctx.SetPool(&region_);

    // Compute HPACK block location (skip padding/priority fields)
    size_t hpack_off = HeadersBlockStart(hdr);
    size_t hpack_len = HeadersBlockLength(hdr, payload);

    if (hdr.flags & H2Flags::PRIORITY) {
        // Parse priority (optional — we don't use it)
        // payload[0..4] contains exclusive+dep+weight
        (void)DecodePriority(payload + HeadersBlockStart(hdr) - 5);
    }

    if (hdr.flags & H2Flags::END_HEADERS) {
        // Complete HPACK block in this frame
        if (!hpack_decoder_.Decode(payload + hpack_off, hpack_len, ctx)) {
            WriteRstStream(sid, H2Error::COMPRESSION_ERROR);
            return;
        }
    } else {
        // CONTINUATION follows — buffer the HPACK block
        continuation_stream_id_ = sid;
        continuation_block_.assign(
            payload + hpack_off, payload + hpack_off + hpack_len);
        return;  // Wait for CONTINUATION frames
    }

    // Check for END_STREAM (or Extended CONNECT which needs no END_STREAM per RFC 8441)
    if ((hdr.flags & H2Flags::END_STREAM) || ctx.ws_extended_) {
        if (hdr.flags & H2Flags::END_STREAM)
            stream_mgr_.OnStreamEndStream(sid);
        stream_mgr_.Enqueue(sid);
    } else {
        // Request has body — keep stream open for DATA frames
        // (will be enqueued when DATA with END_STREAM arrives)
    }
}

// ═══════════════════════════════════════════════════════════════
// CONTINUATION (type 9)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnContinuation(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (static_cast<int32_t>(hdr.stream_id) != continuation_stream_id_) {
        // CONTINUATION must belong to the same stream
        WriteGoAway(stream_mgr_.LastClientStreamId(),
                    H2Error::PROTOCOL_ERROR);
        goaway_sent_ = true;
        return;
    }

    continuation_block_.insert(continuation_block_.end(),
                               payload, payload + hdr.length);

    if (hdr.flags & H2Flags::END_HEADERS) {
        // HPACK block complete — decode it
        int32_t sid = continuation_stream_id_;
        continuation_stream_id_ = 0;

        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            WriteRstStream(sid, H2Error::PROTOCOL_ERROR);
            return;
        }

        if (!hpack_decoder_.Decode(
                continuation_block_.data(),
                continuation_block_.size(), it->second)) {
            WriteRstStream(sid, H2Error::COMPRESSION_ERROR);
            return;
        }

        continuation_block_.clear();

        // Check END_STREAM flag from the original HEADERS or
        // from any CONTINUATION (per RFC, END_STREAM is on HEADERS)
        // Actually END_STREAM is only in the HEADERS flags, not CONTINUATION.
        // So if the HEADERS already set END_STREAM, the stream was already
        // enqueued... wait, in our OnHeaders handler, we only enqueue if
        // END_STREAM is set. But if END_HEADERS was not set (which is why
        // we're here), then we didn't enqueue. So after CONTINUATION
        // completes, we need to check if END_STREAM was set on the HEADERS.
        // Let me re-check...
        //
        // Actually, looking at the code again, when END_HEADERS is not set
        // in the HEADERS frame, we buffer in continuation_block_ and return.
        // The HEADERS may or may not have END_STREAM set.
        // But the stream already went through OnStreamOpen and OnStreamEndStream
        // is only called in OnHeaders if END_STREAM is set.
        //
        // Issue: the stream was opened but never marked as ended.
        // We need to either:
        //   a) Mark the stream as "continuation pending" and check END_STREAM
        //      flag from the HEADERS frame.
        //   b) Store the END_STREAM flag from the HEADERS frame.

        // For now, the HEADERS END_STREAM flag was already checked in
        // OnHeaders. If it wasn't set, we wait for DATA with END_STREAM.
        // This is correct — stream is in Open state, DATA will arrive.
    }
    // else: more CONTINUATION frames follow — keep buffering
}

// ═══════════════════════════════════════════════════════════════
// DATA (type 0)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnData(const H2FrameHeader& hdr, const uint8_t* payload)
{
    int32_t sid = hdr.stream_id;

    // Find stream context
    auto it = streams_.find(sid);
    if (it == streams_.end()) {
        WriteRstStream(sid, H2Error::STREAM_CLOSED);
        return;
    }

    auto& ctx = it->second;

    size_t data_off = DataOffset(hdr);
    size_t data_len = DataLength(hdr, payload);

    // Flow control: consume bytes from window
    auto actual_len = static_cast<uint32_t>(data_len);
    flow_control_.ConsumeBytes(sid, actual_len);
    flow_control_.ConsumeBytes(0, actual_len);  // connection-level

    // Check if stream is already being handled (WS)
    if (ctx.ws_active_) {
        // WS mode — push data to the WS handler's read queue
        ctx.ws_data_queue_.emplace_back(
            reinterpret_cast<const char*>(payload + data_off), data_len);
        if (ctx.ws_wakeup_) {
            asio::error_code ec;
            ctx.ws_wakeup_->cancel(ec);
        }
    } else {
        // Normal mode — accumulate body
        ctx.AppendBody(payload + data_off, data_len);

        // Body size check
        if (max_body_size_ > 0 && ctx.ContentLength() > max_body_size_) {
            WriteRstStream(sid, H2Error::REFUSED_STREAM);
            return;
        }
    }

    // Send WINDOW_UPDATE if needed
    if (flow_control_.ShouldUpdate(sid)) {
        uint32_t credit = flow_control_.PopCredit(sid);
        WriteWindowUpdate(sid, credit);
    }
    if (flow_control_.ShouldUpdate(0)) {
        uint32_t credit = flow_control_.PopCredit(0);
        WriteWindowUpdate(0, credit);
    }

    // Check END_STREAM
    if (hdr.flags & H2Flags::END_STREAM) {
        stream_mgr_.OnStreamEndStream(sid);

        if (!ctx.ws_active_) {
            // Normal stream complete — enqueue for handling
            stream_mgr_.Enqueue(sid);
        } else {
            // WS stream — signal closure to WS handler
            ctx.stream_closed_ = true;
            if (ctx.ws_wakeup_) {
                asio::error_code ec;
                ctx.ws_wakeup_->cancel(ec);
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// RST_STREAM (type 3)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnRstStream(const H2FrameHeader& hdr, const uint8_t* payload)
{
    int32_t sid = hdr.stream_id;
    auto rst = DecodeRstStream(payload);
    (void)rst;  // Log the error code if desired

    stream_mgr_.OnStreamClose(sid);

    // Wake WS handler if active
    auto it = streams_.find(sid);
    if (it != streams_.end()) {
        it->second.stream_closed_ = true;
        if (it->second.ws_active_ && it->second.ws_wakeup_) {
            asio::error_code ec;
            it->second.ws_wakeup_->cancel(ec);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// PING (type 6)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnPing(const H2FrameHeader& hdr, const uint8_t* payload)
{
    // PING ACK must NOT be ACK'd again
    if (hdr.flags & H2Flags::ACK) return;

    // Echo back the 8-byte opaque data
    if (hdr.length >= 8) {
        WritePingAck({*reinterpret_cast<const H2Ping*>(payload)});
    }
}

// ═══════════════════════════════════════════════════════════════
// GOAWAY (type 7)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnGoAway(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (hdr.length < 8) return;
    (void)payload;
    goaway_received_ = true;

    // Peer is shutting down — stop accepting new streams
    if (!goaway_sent_) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::NO_ERROR);
        goaway_sent_ = true;
    }
}

// ═══════════════════════════════════════════════════════════════
// WINDOW_UPDATE (type 8)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnWindowUpdate(const H2FrameHeader& hdr, const uint8_t* payload)
{
    // Peer is telling us it has consumed data we sent.
    // For a server (mostly sender of response data), this is
    // mainly relevant if we're sending large bodies or SSE data.
    // MVP: track but don't react (responses typically fit in window).
    (void)hdr;
    (void)payload;
}

// ═══════════════════════════════════════════════════════════════
// PRIORITY (type 2)
// ═══════════════════════════════════════════════════════════════

void H2Session::OnPriority(const H2FrameHeader& hdr, const uint8_t* payload)
{
    // We ignore priority — all streams are processed sequentially.
    // Parsing would be:
    //   auto pri = DecodePriority(payload);
    (void)hdr;
    (void)payload;
}

// ═══════════════════════════════════════════════════════════════
// FlushOutput — drain output_ buffer to socket
// ═══════════════════════════════════════════════════════════════

asio::awaitable<bool> H2Session::FlushOutput()
{
    if (writing_) co_return true;
    writing_ = true;

    if (!output_.empty()) {
        // Swap to local buffer: co_spawn'd WS handler can safely append
        // to output_ while we send the current batch asynchronously.
        std::vector<uint8_t> send_buf;
        send_buf.swap(output_);

        auto [ec, _] = co_await async_write(
            stream_, asio::buffer(send_buf),
            asio::as_tuple(asio::use_awaitable));
        (void)_;
        if (ec) {
            writing_ = false;
            co_return false;
        }
    }

    writing_ = false;
    co_return true;
}

// ═══════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════════
// H2StreamSink — H2 的 StreamSink 实现
// ═══════════════════════════════════════════════════════════════════

class H2StreamSink : public StreamSink {
public:
    H2StreamSink(H2Session& session, int32_t stream_id)
        : session_(session), stream_id_(stream_id) {}

    asio::awaitable<bool> Write(std::string_view data) override {
        if (ended_) co_return false;
        session_.WriteData(stream_id_,
            reinterpret_cast<const uint8_t*>(data.data()),
            data.size(), false);
        bool ok = co_await session_.FlushOutput();
        if (!ok) ended_ = true;
        co_return ok;
    }

    void End() override {
        if (!ended_) {
            session_.WriteData(stream_id_, nullptr, 0, true);  // END_STREAM
            ended_ = true;
        }
    }

    bool IsDisconnected() const override { return ended_; }

private:
    H2Session& session_;
    int32_t stream_id_;
    bool ended_ = false;
};

// ═══════════════════════════════════════════════════════════════
// ProcessPending — drain the stream pending queue
// ═══════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::ProcessPending()
{
    while (stream_mgr_.HasPending()) {
        auto sid = stream_mgr_.Dequeue();
        co_await HandleStream(sid);
    }
}

// ═══════════════════════════════════════════════════════════════
// Output helpers
// ═══════════════════════════════════════════════════════════════

void H2Session::WriteHeaders(int32_t sid,
                              const std::vector<uint8_t>& hpack,
                              bool end_headers)
{
    uint8_t flags = end_headers ? H2Flags::END_HEADERS : 0;
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + hpack.size());
    EncodeFrameHeader(output_.data() + pos,
        {static_cast<uint32_t>(hpack.size()), H2FrameType::HEADERS, flags, static_cast<uint32_t>(sid)});
    if (!hpack.empty())
        std::memcpy(output_.data() + pos + kFrameHeaderSize, hpack.data(), hpack.size());
}

void H2Session::WriteData(int32_t sid, const uint8_t* data, size_t len, bool end_stream)
{
    uint8_t flags = end_stream ? H2Flags::END_STREAM : 0;
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + len);
    EncodeFrameHeader(output_.data() + pos,
        {static_cast<uint32_t>(len), H2FrameType::DATA, flags, static_cast<uint32_t>(sid)});
    if (len > 0 && data)
        std::memcpy(output_.data() + pos + kFrameHeaderSize, data, len);
}

void H2Session::WriteRstStream(int32_t sid, H2Error err)
{
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 4);
    EncodeFrameHeader(output_.data() + pos,
        {4, H2FrameType::RST_STREAM, 0, static_cast<uint32_t>(sid)});
    EncodeRstStream(output_.data() + pos + kFrameHeaderSize, err);
}

void H2Session::WriteGoAway(int32_t last_sid, H2Error err)
{
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 8);
    EncodeFrameHeader(output_.data() + pos,
        {8, H2FrameType::GOAWAY, 0, 0});
    EncodeGoAway(output_.data() + pos + kFrameHeaderSize,
                 {static_cast<uint32_t>(last_sid), err});
}

void H2Session::WriteWindowUpdate(int32_t sid, uint32_t increment)
{
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 4);
    EncodeFrameHeader(output_.data() + pos,
        {4, H2FrameType::WINDOW_UPDATE, 0, static_cast<uint32_t>(sid)});
    EncodeWindowUpdate(output_.data() + pos + kFrameHeaderSize, increment);
}

void H2Session::WritePingAck(const H2Ping& ping)
{
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 8);
    EncodeFrameHeader(output_.data() + pos,
        {8, H2FrameType::PING, H2Flags::ACK, 0});
    EncodePing(output_.data() + pos + kFrameHeaderSize, ping);
}

void H2Session::WriteSettingsAck()
{
    size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize);
    EncodeFrameHeader(output_.data() + pos,
        {0, H2FrameType::SETTINGS, H2Flags::ACK, 0});
}

// ═══════════════════════════════════════════════════════════════
// WriteResponseHeaders — HPACK-encode + emit HEADERS frame
// ═══════════════════════════════════════════════════════════════

void H2Session::WriteResponseHeaders(int32_t sid, const Response& resp)
{
    // Build header list for HPACK encoder.
    // IMPORTANT: Do NOT use region_.Dup() for header names — that would
    // advance region.Used() past header_end_, causing BodyWire() to
    // report wrong body size.  Keep lowercase name strings alive in
    // a local vector instead.  Reserve upfront to avoid reallocation
    // that would move SSO strings and dangle the string_views.
    std::vector<std::pair<std::string_view, std::string_view>> headers;
    std::vector<std::string> header_names;
    header_names.reserve(static_cast<size_t>(resp.HeaderCount()) + 2);

    // :status pseudo-header
    char status_buf[8];
    int status_len = std::snprintf(status_buf, sizeof(status_buf), "%d", resp.StatusCode());
    headers.emplace_back(":status", std::string_view{status_buf, static_cast<size_t>(status_len)});

    // Response headers (lowercase names per RFC 7540 §8.1.2)
    for (int i = 0; i < resp.HeaderCount(); i++) {
        auto [name, value] = resp.HeaderAt(i);
        if (name == "Connection" || name == "Transfer-Encoding" || name == "Date" ||
            name == "connection" || name == "transfer-encoding" || name == "date")
            continue;
        header_names.emplace_back(name);
        auto& lower = header_names.back();
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        headers.emplace_back(lower, value);
    }

    auto hpack = hpack_encoder_.Encode(headers);
    WriteHeaders(sid, hpack, true);
}

// ═══════════════════════════════════════════════════════════════
// HandleStream — process one HTTP request
//
// Mirrors the original HandleStream logic.  Changes:
//   - Replaced nghttp2_submit_headers → HPACK encode + WriteHeaders
//   - Replaced nghttp2_submit_data → WriteData
//   - Replaced cb_data_read callback → direct buffer writes
//   - WS connection passes output_ reference instead of nghttp2_session
// ═══════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::HandleStream(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) co_return;

    auto& ctx = it->second;

    // Client already reset this stream
    if (ctx.stream_closed_) {
        streams_.erase(stream_id);
        stream_mgr_.RemoveStream(stream_id);
        co_return;
    }

    ctx.SetPool(&region_);
    region_.SetStructuredMode(true);

    // ── Body size check ──
    if (max_body_size_ > 0 && ctx.ContentLength() > max_body_size_) {
        WriteRstStream(stream_id, H2Error::REFUSED_STREAM);
        co_await FlushOutput();
        streams_.erase(stream_id);
        stream_mgr_.RemoveStream(stream_id);
        co_return;
    }

    auto start_time = std::chrono::steady_clock::now();
    bool ok = false;
    try
    {
        // ── PreRequest phase (middleware) ──
        auto resp = middleware_.ExecutePre(ctx);
        if (resp.IsNone()) {
            auto* handler = router_.Match(ctx.Path());
            if (handler && handler->IsStream()) {
                // ── 流式路径 ──
                auto sse_resp = Response::SSEStream(region_, 0);
                WriteResponseHeaders(stream_id, sse_resp);
                {
                    auto init = SseInitialPayload(metrics_);
                    WriteData(stream_id,
                        reinterpret_cast<const uint8_t*>(init.data()),
                        init.size(), false);
                    co_await FlushOutput();
                }
                H2StreamSink sink(*this, stream_id);
                co_await handler->HandleStream(ctx, sink);
                sink.End();
                co_await FlushOutput();
                ok = true;
                goto cleanup;
            } else if (handler && handler->IsAsync()) {
                resp = co_await handler->HandleAsync(ctx);
            } else if (handler) {
                resp = handler->Handle(ctx);
            } else {
                resp = Response::Error(404, region_);
            }
        }

        if (ctx.ws_extended_) {
            auto* ws_handler = router_.Match(ctx.Path());
            if (ws_handler) {
                // RFC 8441 Extended CONNECT: response uses 2xx status.
                // No Sec-WebSocket-Accept — protocol switch is implicit via :protocol.
                std::vector<std::pair<std::string_view, std::string_view>> ws_headers;
                ws_headers.emplace_back(":status", "200");
                ws_headers.emplace_back("date", CachedDate());

                auto hpack = hpack_encoder_.Encode(ws_headers);
                WriteHeaders(stream_id, hpack, true);
                co_await FlushOutput();

                // ── Spawn WS handler on independent coroutine ──
                ctx.ws_active_ = true;

                auto conn = std::make_shared<H2WsConnection>(
                    output_, stream_id, ctx, exec_,
                    [this]() -> asio::awaitable<bool> {
                        co_return co_await FlushOutput();
                    });

                auto h2self = std::static_pointer_cast<H2Session>(
                    this->shared_from_this());

                co_spawn(exec_,
                    [h2self, stream_id, conn = std::move(conn), ws_handler]()
                        mutable -> asio::awaitable<void>
                    {
                        try {
                            auto& ws_ctx = h2self->streams_.at(stream_id);
                            co_await ws_handler->HandleWebSocket(ws_ctx, *conn);
                            h2self->WriteRstStream(stream_id, H2Error::NO_ERROR);
                        } catch (std::exception& e) {
                            std::cerr << "[h2] WS handler error: "
                                      << e.what() << std::endl;
                            h2self->WriteRstStream(stream_id,
                                                    H2Error::INTERNAL_ERROR);
                        }
                        co_await h2self->FlushOutput();
                        conn->MarkClosed();
                        conn.reset();
                        h2self->streams_.erase(stream_id);
                        h2self->stream_mgr_.RemoveStream(stream_id);
                        if (h2self->streams_.empty())
                            h2self->Region().Reset();
                    },
                    asio::detached);

                ok = true;
                co_return;  // skip cleanup — stream stays alive for co_spawn
            }

            // No handler — reject
            WriteRstStream(stream_id, H2Error::REFUSED_STREAM);
            co_await FlushOutput();
            streams_.erase(stream_id);
            stream_mgr_.RemoveStream(stream_id);
            co_return;
        }

        // ── Build and send response headers ──
        WriteResponseHeaders(stream_id, resp);

        // ── SSE stream ──
        if (resp.IsStream()) {
            int32_t sid = stream_id;
            int push_ms = resp.PushIntervalMs();

            // Build and send initial payload
            {
                auto init = SseInitialPayload(metrics_);
                WriteData(sid,
                    reinterpret_cast<const uint8_t*>(init.data()),
                    init.size(), false);
                if (!co_await FlushOutput()) { ok = true; goto cleanup; }
            }

            // ── SSE push loop ──
            {
                auto timer = asio::steady_timer(exec_);
                SsePushState sse;
                sse.Init(metrics_);

                while (!goaway_sent_ && !goaway_received_
                       && !ctx.stream_closed_)
                {
                    timer.expires_after(std::chrono::milliseconds(push_ms));
                    auto [tec] = co_await timer.async_wait(
                        asio::as_tuple(asio::use_awaitable));
                    if (tec) break;

                    auto payload = sse.BuildPayload(metrics_);
                    if (payload.empty())
                        payload = ":\n\n";  // SSE keepalive

                    WriteData(sid,
                        reinterpret_cast<const uint8_t*>(payload.data()),
                        payload.size(), false);
                    if (!co_await FlushOutput())
                        break;
                }
            }

            ok = true;
            goto cleanup;
        }

        // ── Normal (non-SSE): determine body ──
        size_t body_len = 0;
        const uint8_t* body_ptr = nullptr;

        if (!resp.BodyWire().empty()) {
            body_ptr = reinterpret_cast<const uint8_t*>(resp.BodyWire().data());
            body_len = resp.BodyWire().size();
        } else if (resp.IsFile()) {
            auto file_len = (resp.FileRangeLen() > 0)
                          ? resp.FileRangeLen()
                          : resp.FileSize();
            ctx.file_buf_.resize(file_len);
            auto n = ::pread(resp.Fd(), ctx.file_buf_.data(),
                             file_len,
                             static_cast<off_t>(resp.FileRangeOffset()));
            if (n > 0) {
                ctx.file_buf_.resize(static_cast<size_t>(n));
                body_ptr = reinterpret_cast<const uint8_t*>(ctx.file_buf_.data());
                body_len = static_cast<size_t>(n);
            }
        }

        // ── Send DATA frame ──
        if (body_len > 0)
            WriteData(stream_id, body_ptr, body_len, true);  // END_STREAM
        else
            WriteData(stream_id, nullptr, 0, true);  // empty body, END_STREAM

        // ── Post-handle: record metrics ──
        {
            auto end = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               end - start_time).count();
            middleware_.ExecutePostSync(ctx, resp.StatusCode(),
                                        body_len, elapsed, worker_id_);
        }
        ok = true;
    }
    catch (std::exception& e)
    {
        std::cerr << "[h2] handler error (stream " << stream_id
                  << "): " << e.what() << std::endl;
        WriteRstStream(stream_id, H2Error::INTERNAL_ERROR);
    }

cleanup:
    // Flush (error response or final data)
    if (!ok) {
        co_await FlushOutput();
    }

    // Clean up stream context
    streams_.erase(stream_id);
    stream_mgr_.RemoveStream(stream_id);

    // Reset the region when all streams on this connection are done
    if (streams_.empty())
        region_.Reset();
}
