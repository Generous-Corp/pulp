#pragma once

#include <pulp/timebase/tick.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::timebase {

enum class RatchetError {
    None,
    InvalidInterval,
    InvalidHitCount,
    CapacityExceeded,
    InsufficientResolution,
    InvalidWindow,
    OutputTooSmall,
};

struct RatchetProjectionResult {
    RatchetError error = RatchetError::None;
    std::size_t event_count = 0;

    constexpr explicit operator bool() const noexcept {
        return error == RatchetError::None;
    }
};

namespace detail {

inline constexpr std::uint64_t kSignedTickOrderBit = std::uint64_t{1} << 63U;

constexpr std::uint64_t ordered_tick_coordinate(TickPosition position) noexcept {
    return std::bit_cast<std::uint64_t>(position.value) ^ kSignedTickOrderBit;
}

constexpr TickPosition tick_from_ordered_coordinate(std::uint64_t coordinate) noexcept {
    return {std::bit_cast<std::int64_t>(coordinate ^ kSignedTickOrderBit)};
}

class RatchetSubdivisionCursor {
  public:
    constexpr RatchetSubdivisionCursor(TickPosition interval_begin, TickPosition interval_end,
                                       std::uint64_t hit_count) noexcept
        : coordinate_(ordered_tick_coordinate(interval_begin)),
          step_((ordered_tick_coordinate(interval_end) - coordinate_) / hit_count),
          remainder_((ordered_tick_coordinate(interval_end) - coordinate_) % hit_count),
          hit_count_(hit_count) {}

    constexpr TickPosition position() const noexcept {
        return tick_from_ordered_coordinate(coordinate_);
    }

    constexpr void advance() noexcept {
        coordinate_ += step_;
        if (error_ >= hit_count_ - remainder_) {
            ++coordinate_;
            error_ -= hit_count_ - remainder_;
        } else {
            error_ += remainder_;
        }
    }

  private:
    std::uint64_t coordinate_ = 0;
    std::uint64_t step_ = 0;
    std::uint64_t remainder_ = 0;
    std::uint64_t hit_count_ = 1;
    std::uint64_t error_ = 0;
};

} // namespace detail

/// Projects a bounded ratchet burst into an exact clock interval.
///
/// `hit_count` includes the onset at `interval_begin`; `interval_end` belongs
/// to the adjacent clock interval and is never emitted. Integer remainders are
/// distributed across the interval without accumulating a floating-point
/// phase. The same interval and half-open windows therefore produce the same
/// events regardless of callback partition. The operation is allocation-free,
/// bounded by `MaximumHits`, and leaves `output` unchanged on every error.
template <std::size_t MaximumHits = 64>
[[nodiscard]] constexpr RatchetProjectionResult
project_ratchet_interval(TickPosition interval_begin, TickPosition interval_end,
                         std::size_t hit_count, TickPosition window_begin, TickPosition window_end,
                         std::span<TickPosition> output) noexcept {
    static_assert(MaximumHits > 0, "ratchet capacity must be positive");
    static_assert(MaximumHits <= std::numeric_limits<std::uint64_t>::max(),
                  "ratchet capacity must fit the subdivision domain");

    if (interval_end <= interval_begin)
        return {RatchetError::InvalidInterval, 0};
    if (hit_count == 0)
        return {RatchetError::InvalidHitCount, 0};
    if (hit_count > MaximumHits)
        return {RatchetError::CapacityExceeded, 0};
    const auto interval_span = detail::ordered_tick_coordinate(interval_end) -
                               detail::ordered_tick_coordinate(interval_begin);
    if (static_cast<std::uint64_t>(hit_count) > interval_span)
        return {RatchetError::InsufficientResolution, 0};
    if (window_end < window_begin)
        return {RatchetError::InvalidWindow, 0};

    const auto count_visible = [&]() constexpr noexcept {
        std::size_t event_count = 0;
        detail::RatchetSubdivisionCursor cursor(interval_begin, interval_end,
                                                static_cast<std::uint64_t>(hit_count));
        for (std::size_t index = 0; index < hit_count; ++index) {
            const auto position = cursor.position();
            if (position >= window_begin && position < window_end)
                ++event_count;
            cursor.advance();
        }
        return event_count;
    };

    const auto event_count = count_visible();
    if (event_count > output.size())
        return {RatchetError::OutputTooSmall, 0};

    std::size_t output_index = 0;
    detail::RatchetSubdivisionCursor cursor(interval_begin, interval_end,
                                            static_cast<std::uint64_t>(hit_count));
    for (std::size_t index = 0; index < hit_count; ++index) {
        const auto position = cursor.position();
        if (position >= window_begin && position < window_end)
            output[output_index++] = position;
        cursor.advance();
    }
    return {RatchetError::None, event_count};
}

} // namespace pulp::timebase
