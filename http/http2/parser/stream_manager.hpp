#pragma once
#include <cstdint>
#include <unordered_map>
#include <deque>

// ── HTTP/2 stream states (RFC 7540 §5.1) ──
//
// For a server receiving client requests, the typical path is:
//   idle → open → half_closed_remote → closed
//
// WebSocket (RFC 8441) stays open for bidirectional DATA, then closed.
//
enum class H2StreamState : uint8_t {
    Idle,
    Open,
    HalfClosedRemote,   // client sent END_STREAM
    HalfClosedLocal,    // server sent END_STREAM / RST_STREAM
    Closed,
};

// ═══════════════════════════════════════════════════════════════
// H2StreamManager — stream lifecycle, ID validation, pending queue
//
// Owns the stream state machine and the sequential-processing queue.
// Does NOT own per-stream HTTP context data (that stays in
// H2StreamContext, managed by the Session).
// ═══════════════════════════════════════════════════════════════

class H2StreamManager {
public:
    H2StreamManager() = default;

    // ── Stream lifecycle ──

    /// A new HEADERS frame arrived for a client-initiated stream.
    /// Validates stream_id (odd, monotonically increasing).
    /// Returns false on protocol error.
    bool OnStreamOpen(int32_t stream_id);

    /// Client set END_STREAM on HEADERS or DATA.
    void OnStreamEndStream(int32_t stream_id);

    /// RST_STREAM received or server-initiated close.
    void OnStreamClose(int32_t stream_id);

    /// Remove a fully closed stream.
    void RemoveStream(int32_t stream_id);

    /// Current state of a stream.
    H2StreamState GetState(int32_t stream_id) const;
    bool IsActive(int32_t stream_id) const;

    // ── Pending queue ──

    /// Add a complete request stream to the sequential processing queue.
    void Enqueue(int32_t stream_id);

    /// Pop the next stream to process.  Returns 0 if queue is empty.
    int32_t Dequeue();

    /// Number of streams awaiting processing.
    size_t PendingCount() const { return pending_.size(); }
    bool   HasPending()  const { return !pending_.empty(); }

    // ── Limits ──

    void  SetMaxConcurrent(uint32_t max) { max_concurrent_ = max; }
    uint32_t MaxConcurrent() const { return max_concurrent_; }
    bool     CanCreateStream() const;

    /// Total active streams (open + half-closed).
    uint32_t ActiveCount() const { return active_count_; }

    /// Highest stream ID seen from client (used for GOAWAY).
    int32_t LastClientStreamId() const { return last_client_stream_id_; }

    // ── Cleanup ──

    /// Remove all streams whose state is Closed.
    void GcClosed();

private:
    std::unordered_map<int32_t, H2StreamState> states_;
    std::deque<int32_t> pending_;

    uint32_t max_concurrent_ = 100;
    uint32_t active_count_ = 0;
    int32_t  last_client_stream_id_ = 0;
};
