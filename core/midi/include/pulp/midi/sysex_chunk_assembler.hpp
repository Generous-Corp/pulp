#pragma once

/// @file sysex_chunk_assembler.hpp
/// Joins the buffers a driver returns for one long System Exclusive message.
///
/// The classic Windows multimedia input hands back a receive buffer when it is
/// full or when the closing F7 arrives. A message longer than one buffer
/// therefore comes back as several buffers: the first starts with F0, the rest
/// are continuation bytes, and only the last ends with F7. Delivering each
/// buffer on its own would split one message into fragments, so the
/// assembler holds bytes until the F7 shows up.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::midi {

class SysexChunkAssembler {
public:
    /// Longest message kept. A message that outgrows it is dropped whole, up
    /// to and including its closing F7, instead of being delivered truncated.
    static constexpr std::size_t kMaxMessageBytes = 1u << 20;

    /// Append one returned buffer. Returns true when `message()` now holds a
    /// complete F0...F7 message; read it, then call `take()`.
    bool append(const std::uint8_t* data, std::size_t count) {
        if (count == 0) return false;
        if (complete_) {  // previous message was never taken; discard it
            buffer_.clear();
            complete_ = false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t b = data[i];
            if (b >= 0xF8) continue;  // real-time bytes are not part of the message
            if (b == 0xF0) {
                // A new start discards anything unterminated before it.
                buffer_.clear();
                dropping_ = false;
                buffer_.push_back(b);
                continue;
            }
            if (buffer_.empty() && !dropping_) continue;  // stray bytes outside a message
            if (b == 0xF7) {
                if (dropping_) {
                    dropping_ = false;
                    continue;
                }
                buffer_.push_back(b);
                complete_ = true;
                // The driver ends a buffer at F7, so nothing follows it here.
                return true;
            }
            if (b >= 0x80) {  // another status byte aborts the message
                buffer_.clear();
                dropping_ = false;
                continue;
            }
            if (dropping_) continue;
            if (buffer_.size() >= kMaxMessageBytes) {
                buffer_.clear();
                dropping_ = true;
                continue;
            }
            buffer_.push_back(b);
        }
        return false;
    }

    const std::vector<std::uint8_t>& message() const { return buffer_; }

    /// Take the completed message and reset for the next one.
    std::vector<std::uint8_t> take() {
        std::vector<std::uint8_t> out;
        out.swap(buffer_);
        complete_ = false;
        return out;
    }

    /// True while an F0 has been seen without its F7.
    bool in_progress() const { return !buffer_.empty() && !complete_; }

    void reset() {
        buffer_.clear();
        dropping_ = false;
        complete_ = false;
    }

private:
    std::vector<std::uint8_t> buffer_;
    bool dropping_ = false;
    bool complete_ = false;
};

} // namespace pulp::midi
