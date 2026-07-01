#pragma once
#include "net/ws_frame.hpp"
#include <asio.hpp>

// ═══════════════════════════════════════════════════════════════════
// WsRelay — bidirectional WebSocket frame relay
//
// Relays frames between two streams (e.g., client ↔ upstream).
// One direction reads from `from` and writes to `to`, optionally
// re-masking frames (mask_to=true when relaying client→upstream).
//
// Usage:
//   auto relay_c2u = WsRelayDirectional(client_sock, upstream_sock, true, cancel);
//   auto relay_u2c = WsRelayDirectional(upstream_sock, client_sock, false, cancel);
//   co_await relay_c2u;  // wait for either direction to finish
//   cancel = true;       // cancel the other direction
// ═══════════════════════════════════════════════════════════════════

/// Relay frames from `from` to `to`.
/// When a Close frame is received, it is forwarded and the relay stops.
template<typename FromStream, typename ToStream>
asio::awaitable<void> WsRelayDirectional(
    FromStream& from, ToStream& to,
    bool mask_to,
    std::atomic<bool>& cancel_flag)
{
    while (!cancel_flag.load(std::memory_order_relaxed))
    {
        auto frame = co_await ReadFrame(from);
        if (frame.payload.empty() && !frame.fin)
            break;  // read error or connection closed

        // Forward close frame
        if (frame.opcode == WsOpcode::Close)
        {
            co_await WriteFrame(to, WsOpcode::Close,
                                std::move(frame.payload), true, mask_to);
            cancel_flag.store(true, std::memory_order_relaxed);
            break;
        }

        // Re-mask if needed (relay adds masking when forwarding client→upstream)
        co_await WriteFrame(to, frame.opcode,
                            std::move(frame.payload), frame.fin, mask_to);
    }
}
