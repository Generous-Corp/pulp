#pragma once

/// @file triple_buffer.hpp
/// Lock-free latest-value publication between two threads.

#include <atomic>
#include <array>
#include <cstdint>

namespace pulp::runtime {

/// Lock-free triple buffer for publishing the latest value from a writer
/// thread to a reader thread without blocking either side.
///
/// Use case: swapping large config blobs (wavetable data, IR buffers, meter
/// snapshots) from the main thread to the audio thread. The writer never
/// blocks, and the reader always gets the most recently completed write.
/// No allocation on the read path.
///
/// @tparam T  Value type. Must be default-constructible and assignable.
///
/// @code
/// TripleBuffer<MeterData> meters;
///
/// // Audio thread (writer):
/// meters.write(current_levels);
///
/// // UI thread (reader):
/// const auto& levels = meters.read();
/// draw_meter(levels);
/// @endcode
///
/// @note Exactly one writer and one reader thread. Multiple concurrent
///       writers or readers require external synchronization.
template <typename T>
class TripleBuffer {
public:
    TripleBuffer() = default;

    /// Initialize all three internal buffers with the same value.
    explicit TripleBuffer(const T& initial) {
        buffers_[0] = initial;
        buffers_[1] = initial;
        buffers_[2] = initial;
    }

    /// Publish a new value from the writer thread.
    /// Writes into the back buffer, then atomically swaps it to middle.
    void write(const T& value) {
        auto idx = flags_.load(std::memory_order_acquire);
        int back = back_index(idx);
        buffers_[back] = value;
        std::atomic_thread_fence(std::memory_order_release);
        // Swap back and middle, keep the current front, and mark as new.
        uint8_t new_flags;
        uint8_t old_flags;
        do {
            old_flags = flags_.load(std::memory_order_acquire);
            back = back_index(old_flags);
            int mid = mid_index(old_flags);
            // Swap: new back = old mid, new mid = old back (which has new data).
            // Front stays unchanged until the reader consumes the new value.
            new_flags = make_flags(mid, back, front_index(old_flags)) | kDirtyBit;
        } while (!flags_.compare_exchange_weak(old_flags, new_flags,
                 std::memory_order_acq_rel, std::memory_order_acquire));
    }

    /// Read the latest published value from the reader thread.
    /// If new data is available, atomically swaps middle to front first.
    /// @return Const reference to the front buffer. Valid until the next read().
    const T& read() {
        uint8_t flags = flags_.load(std::memory_order_acquire);
        if (flags & kDirtyBit) {
            for (;;) {
                const int front = front_index(flags);
                const int mid = mid_index(flags);
                const uint8_t new_flags =
                    make_flags(back_index(flags), front, mid) & ~kDirtyBit;
                if (flags_.compare_exchange_weak(flags, new_flags,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    flags = new_flags;
                    break;
                }
                if (!(flags & kDirtyBit)) break;
            }
        }
        return buffers_[front_index(flags)];
    }

private:
    // Flags layout: bits [1:0] = back, [3:2] = mid, [5:4] = front, [7] = dirty
    static constexpr uint8_t kDirtyBit = 0x80;

    static uint8_t make_flags(int back, int mid, int front) {
        return static_cast<uint8_t>((back & 3) | ((mid & 3) << 2) | ((front & 3) << 4));
    }
    static int back_index(uint8_t f)  { return f & 3; }
    static int mid_index(uint8_t f)   { return (f >> 2) & 3; }
    static int front_index(uint8_t f) { return (f >> 4) & 3; }

    std::array<T, 3> buffers_{};
    std::atomic<uint8_t> flags_{make_flags(0, 1, 2)};
};

} // namespace pulp::runtime
