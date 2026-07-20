#pragma once

#include <pulp/timeline/model.hpp>

namespace pulp::timeline::detail {

inline std::optional<ItemLocation>
reactivated_location(const std::optional<ItemLocation>& existing,
                     ItemLocation requested) noexcept {
    if (!existing || existing->active || !existing->has_same_owner(requested))
        return std::nullopt;
    requested.active = true;
    return requested;
}

} // namespace pulp::timeline::detail
