#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::midi::arpeggiator_detail {

constexpr std::int64_t floor_div(std::int64_t numerator,
                                 std::int64_t denominator) noexcept {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return remainder != 0 && numerator < 0 ? quotient - 1 : quotient;
}

constexpr std::int64_t saturating_multiply(std::int64_t lhs,
                                           std::int64_t rhs) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (lhs == 0 || rhs == 0)
        return 0;
    if (lhs == -1)
        return rhs == minimum ? maximum : -rhs;
    if (rhs == -1)
        return lhs == minimum ? maximum : -lhs;
    if (lhs > 0) {
        if (rhs > 0 && lhs > maximum / rhs)
            return maximum;
        if (rhs < 0 && rhs < minimum / lhs)
            return minimum;
    } else {
        if (rhs > 0 && lhs < minimum / rhs)
            return minimum;
        if (rhs < 0 && lhs < maximum / rhs)
            return maximum;
    }
    return lhs * rhs;
}

constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

constexpr long double difference_as_long_double(std::int64_t lhs,
                                                std::int64_t rhs) noexcept {
    // Subtract before conversion when both values have the same sign. Their
    // difference is representable, and this preserves adjacent ticks at the
    // int64 limits on platforms where long double has only 53 mantissa bits.
    if ((lhs < 0) == (rhs < 0))
        return static_cast<long double>(lhs - rhs);
    return static_cast<long double>(lhs) - static_cast<long double>(rhs);
}

inline std::int64_t saturating_round(long double value) noexcept {
    constexpr auto minimum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto maximum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (!std::isfinite(value) || value >= maximum)
        return value < 0 ? std::numeric_limits<std::int64_t>::min()
                         : std::numeric_limits<std::int64_t>::max();
    if (value <= minimum)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(std::llround(value));
}

constexpr bool distance_exceeds(std::int64_t lhs, std::int64_t rhs,
                                std::uint64_t tolerance) noexcept {
    const auto magnitude = [](std::int64_t value) constexpr {
        return value >= 0 ? static_cast<std::uint64_t>(value)
                          : static_cast<std::uint64_t>(-(value + 1)) + 1;
    };
    if ((lhs < 0) == (rhs < 0)) {
        const auto left = magnitude(lhs);
        const auto right = magnitude(rhs);
        return (left >= right ? left - right : right - left) > tolerance;
    }
    const auto left = magnitude(lhs);
    const auto right = magnitude(rhs);
    return left > std::numeric_limits<std::uint64_t>::max() - right ||
           left + right > tolerance;
}

} // namespace pulp::midi::arpeggiator_detail
