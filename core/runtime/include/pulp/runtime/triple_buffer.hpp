#pragma once

/// @file triple_buffer.hpp
/// Lock-free latest-value publication between two threads.

#include <atomic>
#include <cassert>
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
/// @note Exactly one writer and one reader at a time. The writer owns its
///       back slot index and the reader its front slot index as plain,
///       unsynchronized state, so successive write() calls must be ordered by
///       happens-before (one thread, or a mutex / host dispatch handoff when
///       the writer role moves between threads), and likewise for read().
///       Two overlapping writers or two overlapping readers corrupt slot
///       ownership permanently, not just one frame. Debug builds assert when
///       two calls in the same role overlap.
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
        [[maybe_unused]] const RoleGuard guard(active_roles_, kWriterRole);
        std::forward<Fill>(fill)(buffers_[back_]);
        // One exchange publishes the filled buffer as the middle slot and
        // hands the previous middle slot back to the writer. Release makes the
        // fill visible to the reader's acquire; acquire orders the reader's
        // last use of the returned slot before the writer refills it.
        const auto previous = middle_.exchange(static_cast<std::uint8_t>(back_ | kDirtyBit),
                                               std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    /// Read the latest published value from the reader thread.
    /// If new data is available, atomically swaps middle to front first.
    /// @return Const reference to the front buffer. Valid until the next read().
    const T& read() noexcept {
        [[maybe_unused]] const RoleGuard guard(active_roles_, kReaderRole);
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

    // Debug-only overlap check for the one-writer / one-reader contract. The
    // flag byte is present in every build so the layout does not depend on
    // NDEBUG; release builds never touch it.
    static constexpr std::uint8_t kWriterRole = 0x01;
    static constexpr std::uint8_t kReaderRole = 0x02;
    struct RoleGuard {
#ifndef NDEBUG
        RoleGuard(std::atomic<std::uint8_t>& roles, std::uint8_t role) noexcept
            : roles_(roles), role_(role) {
            const auto before = roles_.fetch_or(role_, std::memory_order_acquire);
            assert((before & role_) == 0 && "TripleBuffer: two overlapping calls in the same role");
            (void)before;
        }
        ~RoleGuard() {
            roles_.fetch_and(static_cast<std::uint8_t>(~role_), std::memory_order_release);
        }
        std::atomic<std::uint8_t>& roles_;
        std::uint8_t role_;
#else
        RoleGuard(std::atomic<std::uint8_t>&, std::uint8_t) noexcept {}
#endif
    };

    std::array<T, 3> buffers_{};
    std::atomic<std::uint8_t> middle_{1};
    std::uint8_t back_ = 0;
    std::uint8_t front_ = 2;
    std::atomic<std::uint8_t> active_roles_{0};
};

} // namespace pulp::runtime
