#pragma once

#include <pulp/timebase/coordinate_random.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::timebase {

/// An exact probability ratio for an authored trigger.
///
/// A zero numerator never selects a trigger and equal numerator/denominator
/// always selects it. Projection consumes caller-supplied random words, so the
/// grid owns no mutable RNG state and remains invariant under block partition.
struct TriggerProbability {
    std::uint64_t numerator = 1;
    std::uint64_t denominator = 1;
    constexpr auto operator<=>(const TriggerProbability&) const = default;
};

/// The authored data at one track and step coordinate.
struct TriggerCell {
    bool enabled = false;
    std::uint8_t velocity = 100;
    TriggerProbability probability{};
    TickDuration microtiming{};
    constexpr auto operator<=>(const TriggerCell&) const = default;
};

struct TriggerEvent {
    TickPosition position{};
    std::size_t track = 0;
    std::size_t step = 0;
    std::uint8_t velocity = 0;
    constexpr auto operator<=>(const TriggerEvent&) const = default;
};

enum class TriggerGridError {
    None,
    NotConfigured,
    InvalidDimensions,
    CapacityExceeded,
    InvalidStepDuration,
    PatternSpanOutOfRange,
    IndexOutOfRange,
    InvalidVelocity,
    InvalidProbability,
    InvalidMicrotiming,
    InvalidWindow,
    DrawCountMismatch,
    OutputTooSmall,
};

struct TriggerProjectionResult {
    TriggerGridError error = TriggerGridError::None;
    std::size_t event_count = 0;

    constexpr explicit operator bool() const noexcept {
        return error == TriggerGridError::None;
    }
};

/// Fixed-capacity authored trigger data with allocation-free tick projection.
///
/// This is a document-scale pattern value, not a transport or transform chain.
/// The caller supplies the cycle origin, projection window, and one stable
/// random word per configured coordinate. Events are emitted in step-major,
/// then track-major order. Microtiming is bounded so adjacent steps cannot
/// reverse, although saturation at a signed tick endpoint can make positions
/// equal.
template <std::size_t MaxTracks = 16, std::size_t MaxSteps = 64> class TriggerGrid {
    static_assert(MaxTracks > 0);
    static_assert(MaxSteps > 0);
    static_assert(MaxSteps <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    static_assert(MaxTracks <= std::numeric_limits<std::size_t>::max() / MaxSteps);

  public:
    static constexpr std::size_t maximum_tracks = MaxTracks;
    static constexpr std::size_t maximum_steps = MaxSteps;
    static constexpr std::size_t maximum_events = MaxTracks * MaxSteps;

    constexpr TriggerGridError configure(std::size_t track_count, std::size_t step_count,
                                         TickDuration step_duration) noexcept {
        if (track_count == 0 || step_count == 0)
            return TriggerGridError::InvalidDimensions;
        if (track_count > MaxTracks || step_count > MaxSteps)
            return TriggerGridError::CapacityExceeded;
        if (step_duration.value <= 0)
            return TriggerGridError::InvalidStepDuration;

        const auto last_step = step_count - 1;
        const auto maximum_microtiming = (step_duration.value - 1) / 2;
        const auto maximum_tick = std::numeric_limits<std::int64_t>::max();
        if (last_step != 0 && step_duration.value > (maximum_tick - maximum_microtiming) /
                                                        static_cast<std::int64_t>(last_step))
            return TriggerGridError::PatternSpanOutOfRange;

        track_count_ = track_count;
        step_count_ = step_count;
        step_duration_ = step_duration;
        for (auto& cell : cells_)
            cell = {};
        return TriggerGridError::None;
    }

    constexpr std::size_t track_count() const noexcept {
        return track_count_;
    }
    constexpr std::size_t step_count() const noexcept {
        return step_count_;
    }
    constexpr std::size_t coordinate_count() const noexcept {
        return track_count_ * step_count_;
    }
    constexpr TickDuration step_duration() const noexcept {
        return step_duration_;
    }

    constexpr TickDuration minimum_microtiming() const noexcept {
        return {-step_duration_.value / 2};
    }
    constexpr TickDuration maximum_microtiming() const noexcept {
        return {(step_duration_.value - 1) / 2};
    }

    constexpr TriggerGridError set_cell(std::size_t track, std::size_t step,
                                        TriggerCell cell) noexcept {
        if (!contains(track, step))
            return TriggerGridError::IndexOutOfRange;
        if (cell.velocity == 0 || cell.velocity > 127)
            return TriggerGridError::InvalidVelocity;
        if (cell.probability.denominator == 0 ||
            cell.probability.numerator > cell.probability.denominator)
            return TriggerGridError::InvalidProbability;
        if (cell.microtiming < minimum_microtiming() || cell.microtiming > maximum_microtiming())
            return TriggerGridError::InvalidMicrotiming;
        cells_[index_of(track, step)] = cell;
        return TriggerGridError::None;
    }

    constexpr const TriggerCell* cell(std::size_t track, std::size_t step) const noexcept {
        return contains(track, step) ? &cells_[index_of(track, step)] : nullptr;
    }

    constexpr void clear() noexcept {
        for (auto& cell : cells_)
            cell = {};
    }

    /// Projects this cycle into `[window_begin, window_end)` without allocation.
    ///
    /// `probability_draws` must contain exactly one word per configured cell in
    /// step-major, then track-major order. Validation and event counting finish
    /// before `output` is written, so every error leaves it unchanged.
    constexpr TriggerProjectionResult
    project_window(TickPosition cycle_start, TickPosition window_begin, TickPosition window_end,
                   std::span<const std::uint64_t> probability_draws,
                   std::span<TriggerEvent> output) const noexcept {
        if (track_count_ == 0)
            return {TriggerGridError::NotConfigured, 0};
        if (window_end < window_begin)
            return {TriggerGridError::InvalidWindow, 0};
        if (probability_draws.size() != coordinate_count())
            return {TriggerGridError::DrawCountMismatch, 0};

        std::size_t event_count = 0;
        for (std::size_t step = 0; step < step_count_; ++step) {
            for (std::size_t track = 0; track < track_count_; ++track) {
                const auto coordinate = step * track_count_ + track;
                const auto& authored = cells_[index_of(track, step)];
                const auto position = position_at(cycle_start, step, authored.microtiming);
                if (authored.enabled &&
                    selects(authored.probability, probability_draws[coordinate]) &&
                    position >= window_begin && position < window_end)
                    ++event_count;
            }
        }
        if (event_count > output.size())
            return {TriggerGridError::OutputTooSmall, 0};

        std::size_t output_index = 0;
        for (std::size_t step = 0; step < step_count_; ++step) {
            for (std::size_t track = 0; track < track_count_; ++track) {
                const auto coordinate = step * track_count_ + track;
                const auto& authored = cells_[index_of(track, step)];
                const auto position = position_at(cycle_start, step, authored.microtiming);
                if (authored.enabled &&
                    selects(authored.probability, probability_draws[coordinate]) &&
                    position >= window_begin && position < window_end) {
                    output[output_index++] = {position, track, step, authored.velocity};
                }
            }
        }
        return {TriggerGridError::None, event_count};
    }

  private:
    constexpr bool contains(std::size_t track, std::size_t step) const noexcept {
        return track < track_count_ && step < step_count_;
    }

    static constexpr bool selects(TriggerProbability probability, std::uint64_t draw) noexcept {
        if (probability.numerator == 0)
            return false;
        if (probability.numerator == probability.denominator)
            return true;
        return detail::multiply_high(draw, probability.denominator) < probability.numerator;
    }

    constexpr std::size_t index_of(std::size_t track, std::size_t step) const noexcept {
        return track * MaxSteps + step;
    }

    constexpr TickPosition position_at(TickPosition cycle_start, std::size_t step,
                                       TickDuration microtiming) const noexcept {
        // configure() proves this offset is representable. Combining it before
        // the saturating add preserves event order near either signed tick rail.
        const auto offset =
            static_cast<std::int64_t>(step) * step_duration_.value + microtiming.value;
        return {detail::saturating_add(cycle_start.value, offset)};
    }

    std::array<TriggerCell, maximum_events> cells_{};
    std::size_t track_count_ = 0;
    std::size_t step_count_ = 0;
    TickDuration step_duration_{};
};

} // namespace pulp::timebase
