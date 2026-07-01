#include "http/http2/parser/flow_control.hpp"
#include <algorithm>

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

H2FlowControl::H2FlowControl(uint32_t initial_window)
    : initial_window_size_(initial_window)
{
    conn_.credit = static_cast<int32_t>(initial_window);
    conn_.consumed = 0;
}

void H2FlowControl::SetInitialWindow(uint32_t size)
{
    int32_t delta = static_cast<int32_t>(size) - static_cast<int32_t>(initial_window_size_);
    initial_window_size_ = size;

    // Adjust connection window
    conn_.credit += delta;

    // Adjust all existing stream windows
    for (auto& [id, ws] : streams_)
        ws.credit += delta;
}

void H2FlowControl::SetPeerInitialWindow(uint32_t size)
{
    // Actually, this is the same as SetInitialWindow from our perspective.
    // When the peer sends SETTINGS with INITIAL_WINDOW_SIZE, it affects
    // streams that haven't been opened yet (or all, with the delta adjustment).
    SetInitialWindow(size);
}

// ═══════════════════════════════════════════════════════════════
// Window state access
// ═══════════════════════════════════════════════════════════════

H2FlowControl::WindowState& H2FlowControl::GetOrCreate(uint32_t stream_id)
{
    if (stream_id == 0)
        return conn_;

    auto it = streams_.find(stream_id);
    if (it != streams_.end())
        return it->second;

    // First time we hear about this stream — initialise
    auto& ws = streams_[stream_id];
    ws.credit = static_cast<int32_t>(initial_window_size_);
    ws.consumed = 0;
    return ws;
}

// ═══════════════════════════════════════════════════════════════
// Consume + credit tracking
// ═══════════════════════════════════════════════════════════════

void H2FlowControl::ConsumeBytes(uint32_t stream_id, uint32_t n)
{
    auto& ws = GetOrCreate(stream_id);
    ws.credit -= static_cast<int32_t>(n);
    ws.consumed += n;

    // Also consume from connection window
    if (stream_id != 0) {
        conn_.credit -= static_cast<int32_t>(n);
        conn_.consumed += n;
    }
}

bool H2FlowControl::ShouldUpdate(uint32_t stream_id) const
{
    if (stream_id == 0)
        return conn_.consumed >= initial_window_size_ / 2;

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    return it->second.consumed >= initial_window_size_ / 2;
}

uint32_t H2FlowControl::PopCredit(uint32_t stream_id)
{
    auto& ws = GetOrCreate(stream_id);
    uint32_t credit = ws.consumed;
    ws.consumed = 0;
    ws.credit += static_cast<int32_t>(credit);
    return credit;
}

uint32_t H2FlowControl::Available(uint32_t stream_id) const
{
    if (stream_id == 0) {
        uint32_t c = conn_.credit > 0
            ? static_cast<uint32_t>(conn_.credit) : 0;
        // Take min with stream's actual credit
        return c;
    }

    auto it = streams_.find(stream_id);
    if (it == streams_.end())
        return initial_window_size_;

    // Effective window = min(stream, connection)
    uint32_t stream_c = it->second.credit > 0
        ? static_cast<uint32_t>(it->second.credit) : 0;
    uint32_t conn_c = conn_.credit > 0
        ? static_cast<uint32_t>(conn_.credit) : 0;

    return std::min(stream_c, conn_c);
}
