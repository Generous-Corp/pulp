#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timebase/tick.hpp>

#include <bit>
#include <compare>
#include <cstdint>

namespace pulp::timebase {

// Stable musical coordinates replace mutable callback-local RNG state. The
// same event therefore receives the same value regardless of block partition.
struct RandomCoordinate {
    TickPosition tick{};
    std::uint64_t lane = 0;
    std::uint64_t cycle = 0;
    std::uint64_t stream = 0;
    constexpr auto operator<=>(const RandomCoordinate&) const = default;
};

namespace detail {

constexpr std::uint64_t mix_coordinate(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

constexpr std::uint64_t multiply_high(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    const auto lhs_low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lhs));
    const auto lhs_high = lhs >> 32U;
    const auto rhs_low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(rhs));
    const auto rhs_high = rhs >> 32U;
    const auto low_low = lhs_low * rhs_low;
    const auto low_high = lhs_low * rhs_high;
    const auto high_low = lhs_high * rhs_low;
    const auto high_high = lhs_high * rhs_high;
    const auto middle = (low_low >> 32U) + static_cast<std::uint32_t>(low_high) +
                        static_cast<std::uint32_t>(high_low);
    return high_high + (low_high >> 32U) + (high_low >> 32U) + (middle >> 32U);
}

} // namespace detail

constexpr std::uint64_t coordinate_random(std::uint64_t seed,
                                          RandomCoordinate coordinate) noexcept {
    auto value = detail::mix_coordinate(seed);
    value = detail::mix_coordinate(value ^ std::bit_cast<std::uint64_t>(coordinate.tick.value));
    value = detail::mix_coordinate(value ^ coordinate.lane);
    value = detail::mix_coordinate(value ^ coordinate.cycle);
    return detail::mix_coordinate(value ^ coordinate.stream);
}

enum class ProbabilityError {
    InvalidRatio,
};

// Exact probability predicate with no floating-point conversion. `numerator`
// may equal denominator (always) and zero means never.
inline runtime::Result<bool, ProbabilityError>
coordinate_chance(std::uint64_t seed, RandomCoordinate coordinate, std::uint64_t numerator,
                  std::uint64_t denominator) noexcept {
    if (denominator == 0 || numerator > denominator)
        return runtime::Err(ProbabilityError::InvalidRatio);
    if (numerator == 0)
        return runtime::Ok(false);
    if (numerator == denominator)
        return runtime::Ok(true);
    // Multiply-high range reduction avoids modulo's strong low-bit patterns.
    const auto bucket = detail::multiply_high(coordinate_random(seed, coordinate), denominator);
    return runtime::Ok(bucket < numerator);
}

} // namespace pulp::timebase
