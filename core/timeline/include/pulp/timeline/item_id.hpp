#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Nonzero monotonic identity for a Timeline-owned object.
///
/// Zero is the null sentinel. `UINT64_MAX` represents exhausted allocation and
/// is not an object identity.
struct ItemId {
    std::uint64_t value = 0;

    /// Returns whether this value names an object rather than a sentinel.
    constexpr bool valid() const noexcept {
        return value != 0 && value != std::numeric_limits<std::uint64_t>::max();
    }
    constexpr auto operator<=>(const ItemId&) const = default;
};

/// @}

} // namespace pulp::timeline

template <> struct std::hash<pulp::timeline::ItemId> {
    /// Hashes the complete 64-bit identity value.
    std::size_t operator()(pulp::timeline::ItemId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
