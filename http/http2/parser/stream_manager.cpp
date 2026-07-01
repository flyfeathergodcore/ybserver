#include "http/http2/parser/stream_manager.hpp"

// ═══════════════════════════════════════════════════════════════
// Stream lifecycle
// ═══════════════════════════════════════════════════════════════

bool H2StreamManager::OnStreamOpen(int32_t stream_id)
{
    // Client-initiated streams MUST have odd IDs
    if ((stream_id & 1) == 0)
        return false;

    // Stream IDs MUST monotonically increase (no reuse)
    if (stream_id <= last_client_stream_id_)
        return false;

    // Must not exceed max concurrent streams
    if (active_count_ >= max_concurrent_)
        return false;

    last_client_stream_id_ = stream_id;
    states_[stream_id] = H2StreamState::Open;
    active_count_++;
    return true;
}

void H2StreamManager::OnStreamEndStream(int32_t stream_id)
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return;

    // open → half_closed_remote
    if (it->second == H2StreamState::Open)
        it->second = H2StreamState::HalfClosedRemote;
}

void H2StreamManager::OnStreamClose(int32_t stream_id)
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return;

    if (it->second != H2StreamState::Closed) {
        it->second = H2StreamState::Closed;
        if (active_count_ > 0) active_count_--;
    }
}

void H2StreamManager::RemoveStream(int32_t stream_id)
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return;

    if (it->second != H2StreamState::Closed)
        OnStreamClose(stream_id);

    states_.erase(it);
}

// ═══════════════════════════════════════════════════════════════
// State queries
// ═══════════════════════════════════════════════════════════════

H2StreamState H2StreamManager::GetState(int32_t stream_id) const
{
    auto it = states_.find(stream_id);
    return it != states_.end() ? it->second : H2StreamState::Idle;
}

bool H2StreamManager::IsActive(int32_t stream_id) const
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return false;

    return it->second == H2StreamState::Open
        || it->second == H2StreamState::HalfClosedRemote
        || it->second == H2StreamState::HalfClosedLocal;
}

// ═══════════════════════════════════════════════════════════════
// Pending queue
// ═══════════════════════════════════════════════════════════════

void H2StreamManager::Enqueue(int32_t stream_id)
{
    pending_.push_back(stream_id);
}

int32_t H2StreamManager::Dequeue()
{
    if (pending_.empty()) return 0;
    int32_t sid = pending_.front();
    pending_.pop_front();
    return sid;
}

// ═══════════════════════════════════════════════════════════════
// Limits
// ═══════════════════════════════════════════════════════════════

bool H2StreamManager::CanCreateStream() const
{
    return active_count_ < max_concurrent_;
}

// ═══════════════════════════════════════════════════════════════
// Garbage collection
// ═══════════════════════════════════════════════════════════════

void H2StreamManager::GcClosed()
{
    for (auto it = states_.begin(); it != states_.end(); ) {
        if (it->second == H2StreamState::Closed) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}
