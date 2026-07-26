#pragma once

#include <pulp/timebase/tick.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::timebase {

inline constexpr double kBoundaryEpsilonBeats = 1.0e-9;

inline bool valid_sample_rate(double sample_rate) noexcept {
    return sample_rate > 0.0 && std::isfinite(sample_rate);
}

inline bool valid_tempo(double tempo_bpm) noexcept {
    return tempo_bpm > 0.0 && std::isfinite(tempo_bpm);
}

inline bool valid_grid(double grid_beats) noexcept {
    return grid_beats > 0.0 && std::isfinite(grid_beats);
}

inline double beats_per_bar(int numerator, int denominator) noexcept {
    if (numerator <= 0 || denominator <= 0)
        return 0.0;
    return static_cast<double>(numerator) * (4.0 / static_cast<double>(denominator));
}

inline double frames_to_beats(double frames, double sample_rate, double tempo_bpm) noexcept {
    return (frames / sample_rate) * (tempo_bpm / 60.0);
}

inline double beats_to_frames(double beats, double sample_rate, double tempo_bpm) noexcept {
    return (beats * 60.0 / tempo_bpm) * sample_rate;
}

inline double next_grid_boundary(double position_beats, double grid_beats) noexcept {
    const auto index = std::ceil((position_beats - kBoundaryEpsilonBeats) / grid_beats);
    return index * grid_beats;
}

// Swing displaces the interior boundary of every pair of grid cells. In a pair
// spanning `2 * grid` ticks the boundary sits at `grid` when the feel is
// straight; swing moves it to `ratio` of the pair and warps the material on
// either side linearly onto the new halves. The whole passage leans, not only
// the notes that happen to land exactly on the off-beat.
//
// The ratio is an exact rational rather than a double because the result is a
// document-visible tick. Two machines must agree on that tick exactly, and a
// double ratio would make the agreement depend on how a division rounds.
struct SwingRatio {
    std::int64_t numerator = 1;
    std::int64_t denominator = 2;
    constexpr auto operator<=>(const SwingRatio&) const = default;
};

// The straight feel. swing_position() with this ratio is the identity on every
// tick, not merely close to it.
inline constexpr SwingRatio kStraightSwing{1, 2};

// The classic "hard" eighth-note shuffle: the off-beat lands on the last
// triplet of the pair.
inline constexpr SwingRatio kTripletSwing{2, 3};

// The bounds keep every intermediate product inside the signed 64-bit domain.
// The widest is `local * pivot`, and both factors are smaller than a pair.
inline constexpr std::int64_t kMaxSwingGridTicks = 1'000'000'000;
inline constexpr std::int64_t kMaxSwingDenominator = 1'000'000;

inline constexpr bool valid_swing_grid(TickDuration grid) noexcept {
    return grid.value > 0 && grid.value <= kMaxSwingGridTicks;
}

// A ratio of 0 or 1 collapses one half of every pair onto a single instant.
// That is not an extreme feel but a loss of order, so the interval is open at
// both ends rather than clamped into range.
inline constexpr bool valid_swing_ratio(SwingRatio ratio) noexcept {
    return ratio.denominator > 0 && ratio.denominator <= kMaxSwingDenominator &&
           ratio.numerator > 0 && ratio.numerator < ratio.denominator;
}

namespace detail {

constexpr std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) noexcept {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return (remainder != 0 && ((numerator < 0) != (denominator < 0))) ? quotient - 1 : quotient;
}

// Half-up rounding over non-negative operands. Every swing quantity is an
// offset inside one pair, so the negative case does not arise here and no
// policy is invented for it.
constexpr std::int64_t rounded_scale(std::int64_t value, std::int64_t multiplier,
                                     std::int64_t divisor) noexcept {
    return (value * multiplier + divisor / 2) / divisor;
}

// Where the interior boundary lands, in ticks from the start of the pair.
// Clamped away from both ends because a pair only has so many ticks to spend:
// at a coarse tick resolution an extreme ratio would otherwise round onto a
// pair boundary and erase one half of the warp.
constexpr std::int64_t swing_pivot(std::int64_t pair, SwingRatio ratio) noexcept {
    const auto pivot = rounded_scale(pair, ratio.numerator, ratio.denominator);
    return std::clamp<std::int64_t>(pivot, 1, pair - 1);
}

} // namespace detail

// Contract, for a valid grid and ratio:
//   * pair boundaries are exact fixed points, and the grid point inside a pair
//     maps exactly onto the pivot;
//   * the map is non-decreasing, so material is never reordered;
//   * with kStraightSwing it is the identity on every tick, bit for bit.
// It is not injective: the compressed half of each pair has fewer ticks to
// land on than the half it came from, so unswing_position() recovers a
// position only to within the bound that half's slope implies. An invalid grid
// or ratio is the identity — the caller validates, and a bad setting must not
// silently move music.
inline TickPosition swing_position(TickPosition position, TickDuration grid,
                                   SwingRatio ratio) noexcept {
    if (!valid_swing_grid(grid) || !valid_swing_ratio(ratio))
        return position;
    const auto pair = grid.value * 2;
    const auto pivot = detail::swing_pivot(pair, ratio);
    auto local = position.value % pair;
    if (local < 0)
        local += pair;
    const auto warped =
        local < grid.value
            ? detail::rounded_scale(local, pivot, grid.value)
            : pivot + detail::rounded_scale(local - grid.value, pair - pivot, grid.value);
    // Apply the displacement at the original position. Materializing the pair
    // boundary first can underflow below INT64_MIN for a negative, non-boundary
    // tick even though the final displaced position is representable.
    return {detail::saturating_add(position.value, warped - local)};
}

// The left inverse of swing_position() under the same grid and ratio, to
// within the rounding the compressed half forces.
inline TickPosition unswing_position(TickPosition position, TickDuration grid,
                                     SwingRatio ratio) noexcept {
    if (!valid_swing_grid(grid) || !valid_swing_ratio(ratio))
        return position;
    const auto pair = grid.value * 2;
    const auto pivot = detail::swing_pivot(pair, ratio);
    auto local = position.value % pair;
    if (local < 0)
        local += pair;
    const auto straightened =
        local < pivot ? detail::rounded_scale(local, grid.value, pivot)
                      : grid.value + detail::rounded_scale(local - pivot, grid.value, pair - pivot);
    return {detail::saturating_add(position.value, straightened - local)};
}

} // namespace pulp::timebase
