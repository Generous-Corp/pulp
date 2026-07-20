#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct StructuralSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
};

inline constexpr std::uint32_t track_device_chain_version = 2;
inline constexpr std::uint32_t track_automation_lanes_version = 3;

constexpr bool track_has_device_chain(std::uint32_t version) noexcept {
    return version >= track_device_chain_version;
}

constexpr bool track_has_automation_lanes(std::uint32_t version) noexcept {
    return version >= track_automation_lanes_version;
}

inline constexpr StructuralSchemaVersionPolicy track_schema_policy{
    "pulp.timeline.track",
    1,
    track_automation_lanes_version,
};
static_assert(track_schema_policy.oldest_readable_version > 0 &&
              track_schema_policy.oldest_readable_version <=
                  track_schema_policy.current_version);

} // namespace pulp::timeline::detail
