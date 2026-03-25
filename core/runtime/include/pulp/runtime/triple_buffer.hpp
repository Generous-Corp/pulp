#pragma once

// TripleBuffer<T> — Lock-free latest-value publication
//
// Use case: swapping large config blobs (wavetable data, IR buffers, routing
// graphs) from the main thread to the audio thread without blocking either.
//
// Writer: publishes to back buffer, atomically swaps with middle.
// Reader: swaps middle with front if newer, reads from front.
//
// The reader always gets the latest complete value. The writer never blocks.
// No allocation on the read path.

#include <atomic>
#include <array>
#include <cstdint>

namespace pulp::runtime {

template <typename T>
class TripleBuffer {
public:
    TripleBuffer() = default;
    explicit TripleBuffer(const T& initial) {
        buffers_[0] = initial;
        buffers_[1] = initial;
        buffers_[2] = initial;
    }

    // Writer thread: write a new value and publish it.
    void write(const T& value) {
        auto idx = flags_.load(std::memory_order_relaxed);
        int back = back_index(idx);
        buffers_[back] = value;
        // Swap back and middle, mark as new
        uint8_t new_flags;
        uint8_t old_flags;
        do {
            old_flags = flags_.load(std::memory_order_relaxed);
            back = back_index(old_flags);
            int mid = mid_index(old_flags);
            new_flags = make_flags(back_index(old_flags), back, mid_index(old_flags));
            // Swap: new back = old mid, new mid = old back (which has new data), set dirty
            new_flags = make_flags(mid, back, front_index(old_flags)) | kDirtyBit;
        } while (!flags_.compare_exchange_weak(old_flags, new_flags,
                 std::memory_order_release, std::memory_order_relaxed));
    }

    // Reader thread: get the latest published value.
    // Returns a const reference to the front buffer. Valid until next read().
    const T& read() {
        // If dirty, swap front and middle
        uint8_t old_flags = flags_.load(std::memory_order_relaxed);
        if (old_flags & kDirtyBit) {
            uint8_t new_flags;
            do {
                old_flags = flags_.load(std::memory_order_relaxed);
                if (!(old_flags & kDirtyBit)) break;
                int front = front_index(old_flags);
                int mid = mid_index(old_flags);
                // Swap front and middle, clear dirty
                new_flags = make_flags(back_index(old_flags), front, mid) & ~kDirtyBit;
            } while (!flags_.compare_exchange_weak(old_flags, new_flags,
                     std::memory_order_acquire, std::memory_order_relaxed));
        }
        return buffers_[front_index(flags_.load(std::memory_order_acquire))];
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
