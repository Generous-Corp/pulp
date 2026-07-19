#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace pulp::timeline {

struct ItemId {
    std::uint64_t value = 0;

    // Zero is the null sentinel; UINT64_MAX is the exhausted allocator state.
    constexpr bool valid() const noexcept {
        return value != 0 && value != std::numeric_limits<std::uint64_t>::max();
    }
    constexpr auto operator<=>(const ItemId&) const = default;
};

} // namespace pulp::timeline

template <> struct std::hash<pulp::timeline::ItemId> {
    std::size_t operator()(pulp::timeline::ItemId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
