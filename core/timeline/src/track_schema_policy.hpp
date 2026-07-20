#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct StructuralSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
};

inline constexpr StructuralSchemaVersionPolicy track_schema_policy{
    "pulp.timeline.track",
    1,
    3,
};
static_assert(track_schema_policy.oldest_readable_version > 0 &&
              track_schema_policy.oldest_readable_version <=
                  track_schema_policy.current_version);

} // namespace pulp::timeline::detail
