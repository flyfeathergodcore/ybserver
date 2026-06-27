#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

// ── FixedBuffer ──
//
// Small stack-allocated buffer for building HTTP headers and other
// short strings without any heap allocation.  All write operations
// are bounds-checked (silently clamped on overflow).
//
class FixedBuffer {
public:
    static constexpr size_t kCapacity = 512;

    FixedBuffer() : pos_(0) {}

    /// Number of bytes written so far.
    size_t Size() const { return pos_; }
    bool Empty() const { return pos_ == 0; }
    size_t Space() const { return kCapacity - pos_; }

    const char* Data() const { return buf_.data(); }

    /// Read-only view of the written portion.
    std::string_view View() const { return {buf_.data(), pos_}; }

    // ── Write operations ──

    void WriteChar(char c) {
        if (pos_ < kCapacity) buf_[pos_++] = c;
    }

    void Write(std::string_view s) {
        auto n = std::min(s.size(), Space());
        std::memcpy(buf_.data() + pos_, s.data(), n);
        pos_ += n;
    }

    void Write(const char* s) {
        Write(std::string_view(s));
    }

    /// Format an unsigned 64-bit integer without heap allocation.
    void WriteUint(uint64_t n) {
        // Max decimal digits for uint64_t is 20.
        char tmp[24];
        char* p = tmp + sizeof(tmp);
        *--p = '\0';
        if (n == 0) { *--p = '0'; }
        else {
            while (n > 0) {
                *--p = char('0' + (n % 10));
                n /= 10;
            }
        }
        Write(p);
    }

    /// Format a signed 64-bit integer.
    void WriteInt(int64_t n) {
        if (n < 0) {
            WriteChar('-');
            n = -n;
        }
        WriteUint(static_cast<uint64_t>(n));
    }

    void WriteCRLF() {
        Write("\r\n");
    }

    /// Shortcut: "Content-Type: xxx\r\n"
    void WriteContentType(std::string_view mime) {
        Write("Content-Type: ");
        Write(mime);
        WriteCRLF();
    }

    /// Shortcut: "Content-Length: N\r\n"
    void WriteContentLength(size_t n) {
        Write("Content-Length: ");
        WriteUint(n);
        WriteCRLF();
    }

    /// Shortcut: "Connection: keep-alive\r\n"
    void WriteKeepAlive() {
        Write("Connection: keep-alive\r\n");
    }

    /// Shortcut: "Connection: close\r\n"
    void WriteConnectionClose() {
        Write("Connection: close\r\n");
    }

    /// Reset the buffer (does not zero memory, just resets position).
    void Clear() { pos_ = 0; }

    char* WritablePtr() { return buf_.data() + pos_; }

private:
    std::array<char, kCapacity> buf_{};
    size_t pos_;
};
