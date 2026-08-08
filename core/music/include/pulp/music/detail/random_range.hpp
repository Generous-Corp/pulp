#pragma once

#include <cstdint>

namespace pulp::music::detail {

// High half of a 64 x 64-bit product, expressed in 32-bit limbs so the result
// is identical on compilers with and without a native 128-bit integer.
constexpr std::uint64_t multiply_high(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    constexpr std::uint64_t mask = 0xFFFF'FFFFull;
    const auto lhs_low = lhs & mask;
    const auto lhs_high = lhs >> 32u;
    const auto rhs_low = rhs & mask;
    const auto rhs_high = rhs >> 32u;

    const auto low_product = lhs_low * rhs_low;
    auto cross = lhs_high * rhs_low + (low_product >> 32u);
    const auto cross_low = cross & mask;
    const auto cross_high = cross >> 32u;
    cross = lhs_low * rhs_high + cross_low;
    return lhs_high * rhs_high + cross_high + (cross >> 32u);
}

// Maps the full uint64 domain monotonically into [0, exclusive_limit). Callers
// validate that the limit is nonzero before reaching this internal primitive.
constexpr std::uint64_t reduce_random_word(std::uint64_t random_word,
                                           std::uint64_t exclusive_limit) noexcept {
    return multiply_high(random_word, exclusive_limit);
}

} // namespace pulp::music::detail
