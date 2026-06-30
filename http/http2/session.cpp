#include "http/http2/session.hpp"
#include "net/region_pool.hpp"
#include "handler/metrics.hpp"
#include "net/response.hpp"   // for CachedDate declaration
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <openssl/ssl.h>

using asio::ip::tcp;

// ═══════════════════════════════════════════════════════════════
// nghttp2 callback table (lazily created, shared across sessions)
// ═══════════════════════════════════════════════════════════════

nghttp2_session_callbacks* H2Session::GetCallbacks()
{
    static nghttp2_session_callbacks* cb = [] {
        nghttp2_session_callbacks* c = nullptr;
        nghttp2_session_callbacks_new(&c);
        nghttp2_session_callbacks_set_on_begin_headers_callback(
            c, cb_on_begin_headers);
        nghttp2_session_callbacks_set_on_header_callback(
            c, cb_on_header);
        nghttp2_session_callbacks_set_on_frame_recv_callback(
            c, cb_on_frame_recv);
        nghttp2_session_callbacks_set_on_stream_close_callback(
            c, cb_on_stream_close);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
            c, cb_on_data_chunk_recv);
        nghttp2_session_callbacks_set_send_callback(
            c, cb_send);
        return c;
    }();
    return cb;
}

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

    nghttp2_session_server_new(&session_, GetCallbacks(), this);
}

H2Session::~H2Session()
{
    if (session_)
        nghttp2_session_del(session_);
}

// ═══════════════════════════════════════════════════════════════
// Cached HTTP-date (shared with response.cpp)
// ═══════════════════════════════════════════════════════════════

static std::string_view CachedDate()
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
    return {buf, std::strlen(buf)};
}

// ═══════════════════════════════════════════════════════════════
// Start — main coroutine
//
// Event loop:
//   1. Read TLS data from socket
//   2. Feed it to nghttp2 (mem_recv) → nghttp2 fires callbacks
//      → OnFrameRecv adds complete streams to pending_ queue
//   3. Process pending_ queue SEQUENTIALLY via HandleStream
//   4. Flush any pending nghttp2 output frames to socket
//
// Sequential stream processing avoids all concurrency races:
//   - No header callback overwrite (the region callback is set/cleared
//     within a single HandleStream, no interleaving)
//   - No FlushOutput reentrancy (HandleStream runs to completion
//     before the next one starts)
//   - No cb_data_read race (stream context exists until HandleStream
//     erases it, and HandleStream has already finished by then)
// ═══════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::Start()
{
    auto self = this->shared_from_this();
    exec_ = co_await asio::this_coro::executor;

    if (metrics_) metrics_->OnConnectionOpen(worker_id_);

    // Submit initial SETTINGS (required by HTTP/2 connection preface)
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);

    // Send connection preface (SETTINGS frame)
    if (!co_await FlushOutput()) co_return;

    try
    {
        while (nghttp2_session_want_read(session_))
        {
            // ── Greedy accumulate: read all readily available data ──
            // SSL_pending() is a fast in-process check (no syscall).
            // After an async_read_some returns, SSL may have buffered
            // more decrypted data — read it immediately before yielding.
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

                // More decrypted data in SSL buffer?  If so the next read
                // will complete without blocking — get it now.
                if (::SSL_pending(stream_.native_handle()) <= 0)
                    break;
            }
            if (!read_ok) break;

            // ── Feed ALL accumulated data to nghttp2 at once ──
            auto rc = nghttp2_session_mem_recv(session_,
                        read_buf_.data(), read_buf_used_);
            if (rc < 0) {
                std::cerr << "[h2] nghttp2 recv error: " << rc << std::endl;
                break;
            }

            // Process all complete streams sequentially
            while (!pending_.empty()) {
                auto sid = pending_.front();
                pending_.pop_front();
                co_await HandleStream(sid);
            }

            // Flush any pending output frames
            if (!co_await FlushOutput()) break;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "[h2] " << e.what() << std::endl;
    }

    // Graceful GOAWAY
    nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
    co_await FlushOutput();

    if (metrics_) metrics_->OnConnectionClose(worker_id_);
}

// ═══════════════════════════════════════════════════════════════
// FlushOutput — drain nghttp2 output to socket
// ═══════════════════════════════════════════════════════════════

asio::awaitable<bool> H2Session::FlushOutput()
{
    if (writing_) co_return true;
    writing_ = true;

    while (nghttp2_session_want_write(session_))
    {
        output_.clear();

        auto rc = nghttp2_session_send(session_);
        if (rc != 0) {
            std::cerr << "[h2] nghttp2 send error: " << rc << std::endl;
            writing_ = false;
            co_return false;
        }

        if (!output_.empty())
        {
            auto [ec, _] = co_await async_write(
                stream_, asio::buffer(output_),
                asio::as_tuple(asio::use_awaitable));
            (void)_;
            if (ec) {
                writing_ = false;
                co_return false;
            }
        }
    }

    writing_ = false;
    co_return true;
}

// ═══════════════════════════════════════════════════════════════
// nghttp2 callbacks (static → instance forwarding)
// ═══════════════════════════════════════════════════════════════

int H2Session::cb_on_begin_headers(nghttp2_session* session,
                                    const nghttp2_frame* frame,
                                    void* user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    auto* self = static_cast<H2Session*>(user_data);
    self->OnBeginHeaders(frame->hd.stream_id);
    return 0;
}

int H2Session::cb_on_header(nghttp2_session* session,
                             const nghttp2_frame* frame,
                             const uint8_t* name, size_t namelen,
                             const uint8_t* value, size_t valuelen,
                             uint8_t flags, void* user_data)
{
    (void)flags;
    auto* self = static_cast<H2Session*>(user_data);
    self->OnHeader(frame->hd.stream_id,
                   {reinterpret_cast<const char*>(name), namelen},
                   {reinterpret_cast<const char*>(value), valuelen});
    return 0;
}

int H2Session::cb_on_frame_recv(nghttp2_session* session,
                                 const nghttp2_frame* frame,
                                 void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);
    self->OnFrameRecv(frame);
    return 0;
}

int H2Session::cb_on_stream_close(nghttp2_session* session,
                                   int32_t stream_id,
                                   uint32_t error_code,
                                   void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);
    self->OnStreamClose(stream_id);
    (void)error_code;
    return 0;
}

int H2Session::cb_on_data_chunk_recv(nghttp2_session* session,
                                      uint8_t flags,
                                      int32_t stream_id,
                                      const uint8_t* data,
                                      size_t len,
                                      void* user_data)
{
    (void)flags;
    auto* self = static_cast<H2Session*>(user_data);
    self->OnDataChunk(stream_id, data, len);
    return 0;
}

ssize_t H2Session::cb_send(nghttp2_session* session,
                            const uint8_t* data, size_t len,
                            int flags, void* user_data)
{
    (void)session; (void)flags;
    auto* self = static_cast<H2Session*>(user_data);
    self->output_.insert(self->output_.end(), data, data + len);
    return static_cast<ssize_t>(len);
}

ssize_t H2Session::cb_data_read(nghttp2_session* session,
                                 int32_t stream_id,
                                 uint8_t* buf, size_t length,
                                 uint32_t* data_flags,
                                 nghttp2_data_source* source,
                                 void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end())
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;

    auto& ctx = it->second;

    // SSE mode — serve pending payload, defer when empty
    if (ctx.sse_active_) {
        if (!ctx.sse_payload_.empty()) {
            size_t copy = std::min(length, ctx.sse_payload_.size());
            std::memcpy(buf, ctx.sse_payload_.data(), copy);
            ctx.sse_payload_.erase(0, copy);
            return static_cast<ssize_t>(copy);
        }
        return NGHTTP2_ERR_DEFERRED;
    }

    // Normal mode: serve pre-loaded body content
    size_t remaining = ctx.resp_body_len_ - ctx.resp_body_off_;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t copy = std::min(length, remaining);
    std::memcpy(buf, ctx.resp_body_ + ctx.resp_body_off_, copy);
    ctx.resp_body_off_ += copy;
    return static_cast<ssize_t>(copy);
}

// ═══════════════════════════════════════════════════════════════
// Instance handlers
// ═══════════════════════════════════════════════════════════════

void H2Session::OnBeginHeaders(int32_t stream_id)
{
    auto [it, ok] = streams_.try_emplace(stream_id);
    if (ok)
        it->second.SetPool(&region_);
}

void H2Session::OnHeader(int32_t stream_id,
                          std::string_view name,
                          std::string_view value)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    auto& ctx = it->second;

    // Pseudo-headers
    if (name == ":method") {
        ctx.SetMethod(value);
        return;
    }
    if (name == ":path") {
        ctx.SetPath(value);
        return;
    }
    if (name == ":authority" || name == ":scheme")
        return;  // not needed by handler

    // Regular header
    ctx.AddHeader(name, value);
}

void H2Session::OnFrameRecv(const nghttp2_frame* frame)
{
    auto maybeEnqueue = [&](int32_t sid) {
        auto it = streams_.find(sid);
        if (it != streams_.end()) {
            it->second.handled_ = true;
            pending_.push_back(sid);
        }
    };

    if (frame->hd.type == NGHTTP2_HEADERS &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
    {
        // GET/HEAD — no body
        maybeEnqueue(frame->hd.stream_id);
    }
    else if (frame->hd.type == NGHTTP2_DATA &&
             (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
    {
        // POST — body complete
        maybeEnqueue(frame->hd.stream_id);
    }
}

void H2Session::OnStreamClose(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    it->second.stream_closed_ = true;

    // If the stream was in the pending queue but hasn't been handled
    // yet, HandleStream will see stream_closed_ and clean up.
}

void H2Session::OnDataChunk(int32_t stream_id,
                             const uint8_t* data, size_t len)
{
    auto it = streams_.find(stream_id);
    if (it != streams_.end())
        it->second.AppendBody(data, len);
}

// ═══════════════════════════════════════════════════════════════
// HandleStream — process one HTTP request
// ═══════════════════════════════════════════════════════════════

asio::awaitable<void> H2Session::HandleStream(int32_t stream_id)
{
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) co_return;

    auto& ctx = it->second;

    // Client already reset this stream — nothing to serve
    if (ctx.stream_closed_) {
        streams_.erase(stream_id);
        co_return;
    }

    ctx.SetPool(&region_);
    region_.SetStructuredMode(true);

    // ── Body size check ──
    if (max_body_size_ > 0 && ctx.ContentLength() > max_body_size_) {
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE,
                                   stream_id, NGHTTP2_REFUSED_STREAM);
        streams_.erase(stream_id);
        co_return;
    }

    auto start_time = std::chrono::steady_clock::now();
    bool ok = false;
    try
    {
        // ── PreRequest phase (sync) ──
        auto resp = middleware_.ExecutePre(ctx);
        if (resp.IsNone()) {
            // ── Route → Handler ──
            auto* handler = router_.Match(ctx.Path());
            if (handler && handler->IsAsync()) {
                resp = co_await handler->HandleAsync(ctx);
            } else if (handler) {
                resp = handler->Handle(ctx);
            } else {
                resp = Response::Error(404, region_);
            }
        }

        // ── Build nghttp2 response headers (shared by SSE and normal paths) ──
        nv_reuse_.clear();

        // :status (pseudo-header)
        char status_buf[8];
        int status_len = std::snprintf(status_buf, sizeof(status_buf),
                                        "%d", resp.StatusCode());
        {
            nghttp2_nv h;
            h.name = (uint8_t*)":status";
            h.namelen = 7;
            h.value = reinterpret_cast<uint8_t*>(status_buf);
            h.valuelen = static_cast<size_t>(status_len);
            h.flags = NGHTTP2_NV_FLAG_NONE;
            nv_reuse_.push_back(h);
        }

        // date
        auto date = CachedDate();
        {
            nghttp2_nv h;
            h.name = (uint8_t*)"date";
            h.namelen = 4;
            h.value = reinterpret_cast<uint8_t*>(const_cast<char*>(date.data()));
            h.valuelen = date.size();
            h.flags = NGHTTP2_NV_FLAG_NONE;
            nv_reuse_.push_back(h);
        }

        // Response headers — read from Response::HeaderAt()
        for (int i = 0; i < resp.HeaderCount(); i++) {
            auto [name, value] = resp.HeaderAt(i);
            if (name == "connection" || name == "transfer-encoding"
                || name == "content-length" || name == "date")
                continue;
            nghttp2_nv h;
            h.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
            h.namelen = name.size();
            h.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
            h.valuelen = value.size();
            h.flags = NGHTTP2_NV_FLAG_NONE;
            nv_reuse_.push_back(h);
        }

        // ── SSE stream ──
        if (resp.IsStream()) {
            int32_t sid = stream_id;
            ctx.sse_active_ = true;
            ctx.push_interval_ms_ = resp.PushIntervalMs();

            // Build initial payload: retry + full metrics dump
            {
                std::string init;
                init = "retry: 2000\n\n";
                if (metrics_) {
                    auto full_json = metrics_->RenderMetricsJson();
                    init += "event: full\ndata: ";
                    init += full_json;
                    init += "\n\n";
                }
                ctx.sse_payload_ = std::move(init);
            }

            // Submit HEADERS (END_HEADERS, but no END_STREAM — SSE keeps stream open)
            nghttp2_submit_headers(session_, NGHTTP2_FLAG_END_HEADERS,
                                    sid, nullptr,
                                    nv_reuse_.data(), nv_reuse_.size(), nullptr);

            // Submit DATA without END_STREAM — uses cb_data_read for payload
            {
                nghttp2_data_provider provider;
                provider.source.ptr = nullptr;
                provider.read_callback = cb_data_read;
                nghttp2_submit_data(session_, NGHTTP2_FLAG_NONE,
                                     sid, &provider);
            }

            // Flush initial frame(s)
            if (!co_await FlushOutput()) { ok = true; goto cleanup; }

            // ── SSE push loop (timer-driven) ──
            {
                auto timer = asio::steady_timer(exec_);
                int64_t last_ts = 0;
                std::vector<AlertState> prev_alerts;
                if (metrics_) prev_alerts = metrics_->AlertStates();

                while (nghttp2_session_want_read(session_) && !ctx.stream_closed_)
                {
                    timer.expires_after(std::chrono::milliseconds(ctx.push_interval_ms_));
                    auto [tec] = co_await timer.async_wait(
                        asio::as_tuple(asio::use_awaitable));
                    if (tec) break;

                    std::string payload;
                    if (metrics_)
                    {
                        auto delta = metrics_->RenderLatestSnapshot(last_ts);
                        last_ts = metrics_->LastFlushTimestamp();
                        if (!delta.empty()) {
                            payload = "event: metrics\ndata: ";
                            payload += delta;
                            payload += "\n\n";
                        }

                        auto alert_delta = metrics_->RenderAlertDelta(prev_alerts);
                        if (!alert_delta.empty()) {
                            payload += "event: alert\ndata: ";
                            payload += alert_delta;
                            payload += "\n\n";
                        }
                        prev_alerts = metrics_->AlertStates();
                    }

                    if (payload.empty())
                        payload = ":\n\n";  // SSE comment keepalive

                    ctx.sse_payload_ = std::move(payload);
                    nghttp2_session_resume_data(session_, sid);
                    if (!co_await FlushOutput())
                        break;
                }
            }

            ok = true;
            goto cleanup;  // skip the rest, go to cleanup
        }

        // ── Normal (non-SSE): determine body source ──
        size_t body_len = 0;
        if (!resp.BodyWire().empty()) {
            ctx.resp_body_ = resp.BodyWire().data();
            ctx.resp_body_len_ = resp.BodyWire().size();
            body_len = ctx.resp_body_len_;
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
                ctx.resp_body_ = ctx.file_buf_.data();
                ctx.resp_body_len_ = static_cast<size_t>(n);
            }
            body_len = ctx.resp_body_len_;
        }
        ctx.resp_body_off_ = 0;

        // ── Submit HEADERS frame ──
        int32_t sid = stream_id;
        nghttp2_submit_headers(session_, NGHTTP2_FLAG_END_HEADERS,
                                sid, nullptr,
                                nv_reuse_.data(), nv_reuse_.size(), nullptr);

        // ── Submit DATA frame ──
        if (body_len > 0) {
            nghttp2_data_provider provider;
            provider.source.ptr = nullptr;
            provider.read_callback = cb_data_read;
            nghttp2_submit_data(session_, NGHTTP2_FLAG_END_STREAM,
                                 sid, &provider);
        }

        // ── Flush frames to socket ──
        co_await FlushOutput();

        // ── Post-handle: record metrics ──
        {
            auto end = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               end - start_time).count();
            // body_len set above in the normal path; 0 for SSE (skipped via goto)
            co_await middleware_.ExecutePost(ctx, resp.StatusCode(),
                                             body_len, elapsed, worker_id_);
        }
        ok = true;
    }
    catch (std::exception& e)
    {
        std::cerr << "[h2] handler error (stream " << stream_id
                  << "): " << e.what() << std::endl;
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE,
                                   stream_id, NGHTTP2_INTERNAL_ERROR);
    }

cleanup:
    // Flush error response (outside catch block)
    if (!ok) {
        co_await FlushOutput();
    }

    // Clean up stream context
    streams_.erase(stream_id);

    // Reset the region when all streams on this connection are done.
    if (streams_.empty())
        region_.Reset();
}
