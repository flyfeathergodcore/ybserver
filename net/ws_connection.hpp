#pragma once
#include "net/ws_frame.hpp"
#include <asio.hpp>
#include <string>
#include <memory>

// ═══════════════════════════════════════════════════════════════════
// WsConnectionBase — type-erased WebSocket connection interface
//
// Enables RequestHandler::HandleWebSocket(WsConnectionBase&) without
// templating the handler on stream type.
// ═══════════════════════════════════════════════════════════════════

class WsConnectionBase {
public:
    virtual ~WsConnectionBase() = default;

    /// Read next frame (auto-responds ping/pong, auto-replies close).
    /// Returns empty frame on connection close or error.
    virtual asio::awaitable<WsFrame> Read() = 0;

    /// Send a frame.
    virtual asio::awaitable<void> Send(WsOpcode opcode, std::string payload,
                                        bool fin = true) = 0;

    /// Initiate close handshake.
    virtual asio::awaitable<void> Close(uint16_t code = 1000,
                                         std::string_view reason = {}) = 0;

    bool IsOpen() const { return !closed_; }

protected:
    bool closed_ = false;
};

// ═══════════════════════════════════════════════════════════════════
// WsConnection — WebSocket connection state (templated implementation)
//
// Wraps a stream (TCP or SSL) and provides frame-level read/write
// with automatic ping/pong/close handshake handling.
// Template parameter: Stream (e.g., asio::ssl::stream<tcp::socket>)
// ═══════════════════════════════════════════════════════════════════

template<typename Stream>
class WsConnection : public WsConnectionBase {
public:
    explicit WsConnection(Stream& stream)
        : stream_(stream) {}

    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    asio::awaitable<WsFrame> Read() override
    {
        for (;;)
        {
            auto frame = co_await ReadFrame(stream_);
            if (frame.payload.empty() && !frame.fin)
                co_return WsFrame{};  // read error

            switch (frame.opcode)
            {
            case WsOpcode::Ping:
                co_await WriteFrame(stream_, WsOpcode::Pong,
                                    std::move(frame.payload), true, false);
                continue;

            case WsOpcode::Close:
            {
                uint16_t code = 1000;
                if (frame.payload.size() >= 2) {
                    code = (static_cast<uint8_t>(frame.payload[0]) << 8) |
                            static_cast<uint8_t>(frame.payload[1]);
                }
                std::string_view reason;
                if (frame.payload.size() > 2)
                    reason = {frame.payload.data() + 2,
                              frame.payload.size() - 2};
                co_await WriteCloseFrame(stream_, code, reason);
                closed_ = true;
                co_return WsFrame{};
            }

            case WsOpcode::Pong:
                continue;

            default:
                co_return frame;
            }
        }
    }

    asio::awaitable<void> Send(WsOpcode opcode, std::string payload,
                                bool fin = true) override
    {
        co_await WriteFrame(stream_, opcode, std::move(payload), fin, false);
    }

    asio::awaitable<void> Close(uint16_t code = 1000,
                                 std::string_view reason = {}) override
    {
        if (closed_) co_return;
        closed_ = true;
        co_await WriteCloseFrame(stream_, code, reason);
    }

private:
    Stream& stream_;
};
