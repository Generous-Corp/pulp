#pragma once

/// @file triple_buffer.hpp
/// Lock-free latest-value publication between two threads.

#include <atomic>
#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

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
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "TripleBuffer publication flags must be lock-free");

    TripleBuffer() = default;

    /// Initialize all three internal buffers with the same value.
    explicit TripleBuffer(const T& initial) {
        buffers_[0] = initial;
        buffers_[1] = initial;
        buffers_[2] = initial;
    }

    /// Publish a new value from the writer thread.
    /// Writes into the back buffer, then atomically swaps it to middle.
    /// Costs one copy and one atomic exchange; never blocks or retries.
    void write(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        write_with(
            [&value](T& back) noexcept(std::is_nothrow_copy_assignable_v<T>) { back = value; });
    }

    /// Fill and publish the writer-owned back buffer in place.
    ///
    /// This is useful for prepared payloads whose assignment through a
    /// temporary could allocate. The callback runs only on the writer thread
    /// and must not retain the reference. If it throws, no value is published.
    template <typename Fill>
    void write_with(Fill&& fill) noexcept(noexcept(std::declval<Fill>()(std::declval<T&>()))) {
        std::forward<Fill>(fill)(buffers_[back_]);
        // One exchange publishes the filled buffer as the middle slot and
        // hands the previous middle slot back to the writer. Release makes the
        // fill visible to the reader's acquire; acquire orders the reader's
        // last use of the returned slot before the writer refills it.
        const auto previous = middle_.exchange(
            static_cast<std::uint8_t>(back_ | kDirtyBit), std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    /// Read the latest published value from the reader thread.
    /// If new data is available, atomically swaps middle to front first.
    /// @return Const reference to the front buffer. Valid until the next read().
    const T& read() noexcept {
        if (middle_.load(std::memory_order_relaxed) & kDirtyBit) {
            const auto previous = middle_.exchange(front_, std::memory_order_acq_rel);
            front_ = previous & kIndexMask;
        }
        return buffers_[front_];
    }

private:
    // middle_ holds the index of the slot between writer and reader, plus a
    // dirty bit set by the writer and cleared by the reader's swap. back_ is
    // owned by the writer thread and front_ by the reader thread; neither is
    // shared, so neither needs to be atomic.
    static constexpr std::uint8_t kDirtyBit = 0x80;
    static constexpr std::uint8_t kIndexMask = 0x03;

    std::array<T, 3> buffers_{};
    std::atomic<std::uint8_t> middle_{1};
    std::uint8_t back_ = 0;
    std::uint8_t front_ = 2;
};

} // namespace pulp::runtime
