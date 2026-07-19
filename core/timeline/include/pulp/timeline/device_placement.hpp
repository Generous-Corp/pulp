#pragma once

#include <pulp/timeline/item_id.hpp>

#include <compare>

namespace pulp::timeline {

// Stable document identity for one logical placement in a Track device chain.
// Runtime instances, graph nodes, plugin formats, and platform metadata are not
// part of it.
struct DevicePlacement {
    ItemId id;

    constexpr bool valid() const noexcept {
        return id.valid();
    }

    constexpr auto operator<=>(const DevicePlacement&) const = default;
};

} // namespace pulp::timeline
