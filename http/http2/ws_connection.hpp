#pragma once
#include "net/ws_connection.hpp"
#include "http/http2/stream_context.hpp"
#include "http/http2/parser/BFL.hpp"
#include <asio.hpp>
#include <deque>
#include <memory>
#include <functional>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
// H2WsConnection — WebSocket connection over H2 (RFC 8441)
//
// Implements WsConnectionBase for H2 Extended CONNECT streams.
//   - Read()  dequeues from H2StreamContext::ws_data_queue_
//   - Send()  builds a DATA frame directly into the session output buffer
//   - Close() sends END_STREAM via DATA frame with close code
//
// Frame building: uses BFL functions (EncodeFrameHeader) and pushes
// raw bytes into the session's output buffer, then calls the flusher.
// ═══════════════════════════════════════════════════════════════════

class H2WsConnection : public WsConnectionBase,
                        public std::enable_shared_from_this<H2WsConnection> {
public:
    using Flusher = std::function<asio::awaitable<bool>()>;

    H2WsConnection(std::vector<uint8_t>& output, int32_t stream_id,
                   H2StreamContext& ctx, asio::any_io_executor exec,
                   Flusher flusher)
        : output_(output)
        , stream_id_(stream_id)
        , ctx_(ctx)
        , flusher_(std::move(flusher))
        , wake_timer_(exec)
    {
        ctx_.ws_wakeup_ = &wake_timer_;
    }

    ~H2WsConnection() override {
        if (ctx_.ws_wakeup_ == &wake_timer_)
            ctx_.ws_wakeup_ = nullptr;
    }

    H2WsConnection(const H2WsConnection&) = delete;
    H2WsConnection& operator=(const H2WsConnection&) = delete;

    // ── WsConnectionBase ──

    asio::awaitable<WsFrame> Read() override
    {
        while (!closed_ && !ctx_.ws_closed_)
        {
            if (!ctx_.ws_data_queue_.empty()) {
                auto data = std::move(ctx_.ws_data_queue_.front());
                ctx_.ws_data_queue_.pop_front();
                co_return WsFrame{true, WsOpcode::Binary, std::move(data)};
            }

            if (ctx_.stream_closed_) {
                closed_ = true;
                co_return WsFrame{};
            }

            // Yield — wake_timer_ is cancelled by OnData on data arrival
            wake_timer_.expires_after(std::chrono::milliseconds(100));
            asio::error_code ec;
            co_await wake_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
            (void)ec;
        }

        closed_ = true;
        co_return WsFrame{};
    }

    asio::awaitable<void> Send(WsOpcode opcode, std::string payload,
                                bool fin = true) override
    {
        if (closed_ || ctx_.ws_closed_)
            co_return;

        (void)opcode;  // H2 DATA frames carry raw app data, no WS opcode

        uint8_t flags = fin ? H2Flags::END_STREAM : 0;
        size_t pos = output_.size();
        output_.resize(pos + kFrameHeaderSize + payload.size());
        EncodeFrameHeader(output_.data() + pos,
            {static_cast<uint32_t>(payload.size()), H2FrameType::DATA, flags, static_cast<uint32_t>(stream_id_)});
        if (!payload.empty())
            std::memcpy(output_.data() + pos + kFrameHeaderSize, payload.data(), payload.size());

        if (fin) {
            ctx_.ws_closed_ = true;
            closed_ = true;
        }

        if (flusher_)
            co_await flusher_();
    }

    asio::awaitable<void> Close(uint16_t code = 1000,
                                 std::string_view reason = {}) override
    {
        if (closed_ || ctx_.ws_closed_) co_return;
        closed_ = true;
        ctx_.ws_closed_ = true;

        std::string payload;
        payload.push_back(static_cast<char>(code >> 8));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload.append(reason);

        size_t pos = output_.size();
        output_.resize(pos + kFrameHeaderSize + payload.size());
        EncodeFrameHeader(output_.data() + pos,
            {static_cast<uint32_t>(payload.size()), H2FrameType::DATA,
             H2Flags::END_STREAM, static_cast<uint32_t>(stream_id_)});
        if (!payload.empty())
            std::memcpy(output_.data() + pos + kFrameHeaderSize, payload.data(), payload.size());

        if (flusher_)
            co_await flusher_();
    }

    void MarkClosed() {
        closed_ = true;
        ctx_.ws_closed_ = true;
        asio::error_code ec;
        wake_timer_.cancel(ec);
    }

    /// Whether the connection is marked closed.
    bool IsClosed() const { return closed_; }

private:
    std::vector<uint8_t>& output_;
    int32_t stream_id_;
    H2StreamContext& ctx_;
    Flusher flusher_;
    asio::steady_timer wake_timer_;
};
