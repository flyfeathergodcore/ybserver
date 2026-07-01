#pragma once
#include <cstdint>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════
// H2FlowControl — connection-level + per-stream window management
//
// Each stream (and the connection itself) starts with an initial
// window of 65535 bytes.  The peer cannot send more data than
// the available credit.  When the application has consumed the
// data, it sends a WINDOW_UPDATE frame to restore credit.
//
// Strategy (simple, reasonable for MVP):
//   ConsumedBytes() accumulates.  When consumed >= initial/2,
//   ShouldUpdate() returns true, and Credit() tells you how
//   much to write in the WINDOW_UPDATE frame.
// ═══════════════════════════════════════════════════════════════

class H2FlowControl {
public:
    explicit H2FlowControl(uint32_t initial_window = 65535);

    /// Reconfigure initial window size (from SETTINGS).
    void SetInitialWindow(uint32_t size);

    /// Record that @a n bytes have been received and consumed.
    void ConsumeBytes(uint32_t stream_id, uint32_t n);

    /// True when it's time to send a WINDOW_UPDATE for this entity.
    bool ShouldUpdate(uint32_t stream_id) const;

    /// Number of bytes to put in the WINDOW_UPDATE frame.
    /// Calling this resets the consumed counter.
    uint32_t PopCredit(uint32_t stream_id);

    /// Current available credit (for debugging).
    uint32_t Available(uint32_t stream_id) const;

    /// Set peer's INITIAL_WINDOW_SIZE from SETTINGS.
    void SetPeerInitialWindow(uint32_t size);

private:
    struct WindowState {
        int32_t credit;     // available credit (can go negative with SETTINGS changes)
        uint32_t consumed;  // bytes consumed since last WINDOW_UPDATE
    };

    uint32_t initial_window_size_ = 65535;
    WindowState conn_;   // stream_id = 0
    std::unordered_map<uint32_t, WindowState> streams_;

    WindowState& GetOrCreate(uint32_t stream_id);
};
