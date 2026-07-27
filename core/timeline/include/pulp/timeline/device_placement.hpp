#pragma once

#include <pulp/timeline/item_id.hpp>

#include <compare>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Stable document identity for one logical placement in a Track device chain.
///
/// Runtime instances, graph nodes, plugin formats, configuration payloads, and
/// platform metadata are not part of this value.
struct DevicePlacement {
    ItemId id;

    /// Returns whether `id` is a usable, non-sentinel document identity.
    constexpr bool valid() const noexcept {
        return id.valid();
    }

    constexpr auto operator<=>(const DevicePlacement&) const = default;
};

/// @}

} // namespace pulp::timeline
