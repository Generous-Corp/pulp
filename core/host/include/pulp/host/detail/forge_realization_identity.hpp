#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::host::detail {

// Locale-independent, lossless identity token for an already-normalized
// registration-time real value. Hex IEEE bits avoid decimal rounding aliases.
inline std::string realization_real_token(double value) {
    constexpr char digits[] = "0123456789abcdef";
    const auto bits = std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value);
    std::string token(16, '0');
    for (int i = 15; i >= 0; --i)
        token[static_cast<std::size_t>(15 - i)] =
            digits[(bits >> (i * 4)) & 0xfu];
    return token;
}

}  // namespace pulp::host::detail
