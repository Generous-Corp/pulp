#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace pulp::timebase {

namespace detail {

constexpr std::int64_t saturating_add(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs)
        return std::numeric_limits<std::int64_t>::max();
    if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)
        return std::numeric_limits<std::int64_t>::min();
    return lhs + rhs;
}

constexpr std::int64_t saturating_subtract(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs)
        return std::numeric_limits<std::int64_t>::min();
    if (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs)
        return std::numeric_limits<std::int64_t>::max();
    return lhs - rhs;
}

} // namespace detail

inline constexpr std::int64_t kTicksPerQuarter = 705'600;

struct TickDuration {
    std::int64_t value = 0;
    constexpr auto operator<=>(const TickDuration&) const = default;
};

struct TickPosition {
    std::int64_t value = 0;
    constexpr auto operator<=>(const TickPosition&) const = default;
};

// Musical position on the transport's non-looping clock. Timeline positions
// may seek or wrap; MonotonicBeat is advanced independently so scheduled
// intents such as "launch at the next bar" retain a stable clock domain.
// The transport owns that advancement policy. The timebase module supplies
// the strong type so a looping TickPosition cannot be passed accidentally.
struct MonotonicBeat {
    TickPosition position;
    constexpr auto operator<=>(const MonotonicBeat&) const = default;
};

// Position and duration arithmetic is total over the signed 64-bit domain:
// results that exceed it clamp to the nearest endpoint.

constexpr TickPosition operator+(TickPosition position, TickDuration duration) noexcept {
    return {detail::saturating_add(position.value, duration.value)};
}

constexpr TickPosition operator-(TickPosition position, TickDuration duration) noexcept {
    return {detail::saturating_subtract(position.value, duration.value)};
}

constexpr TickDuration operator-(TickPosition lhs, TickPosition rhs) noexcept {
    return {detail::saturating_subtract(lhs.value, rhs.value)};
}

constexpr MonotonicBeat operator+(MonotonicBeat beat, TickDuration duration) noexcept {
    return {{detail::saturating_add(beat.position.value, duration.value)}};
}

constexpr MonotonicBeat operator-(MonotonicBeat beat, TickDuration duration) noexcept {
    return {{detail::saturating_subtract(beat.position.value, duration.value)}};
}

constexpr TickDuration operator-(MonotonicBeat lhs, MonotonicBeat rhs) noexcept {
    return {detail::saturating_subtract(lhs.position.value, rhs.position.value)};
}

} // namespace pulp::timebase
