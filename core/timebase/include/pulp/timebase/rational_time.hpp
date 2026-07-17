#pragma once

#include <compare>
#include <cstdint>
#include <numeric>

namespace pulp::timebase {

struct SamplePosition {
    std::int64_t value = 0;
    constexpr auto operator<=>(const SamplePosition&) const = default;
};

struct RationalRate {
    std::uint64_t numerator = 48'000;
    std::uint64_t denominator = 1;

    constexpr bool valid() const noexcept {
        return numerator != 0 && denominator != 0;
    }

    constexpr RationalRate normalized() const noexcept {
        if (!valid())
            return {0, 1};
        const auto divisor = std::gcd(numerator, denominator);
        return {numerator / divisor, denominator / divisor};
    }

    constexpr long double as_long_double() const noexcept {
        return valid() ? static_cast<long double>(numerator) / static_cast<long double>(denominator)
                       : 0.0L;
    }

    constexpr auto operator<=>(const RationalRate&) const = default;
};

} // namespace pulp::timebase
