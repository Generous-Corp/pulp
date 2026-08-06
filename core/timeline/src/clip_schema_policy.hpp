#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct ClipSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
    std::uint32_t time_conform_introduced_version;
    std::uint32_t fade_shape_introduced_version;

    [[nodiscard]] constexpr bool requires_time_conform(std::uint32_t version) const noexcept {
        return version >= time_conform_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_fade_shape(std::uint32_t version) const noexcept {
        return version >= fade_shape_introduced_version;
    }
};

inline constexpr ClipSchemaVersionPolicy clip_schema_policy{
    "pulp.timeline.clip",
    1,
    3,
    2,
    3,
};

static_assert(clip_schema_policy.oldest_readable_version > 0 &&
              clip_schema_policy.oldest_readable_version <= clip_schema_policy.current_version &&
              !clip_schema_policy.requires_time_conform(
                  clip_schema_policy.time_conform_introduced_version - 1) &&
              clip_schema_policy.requires_time_conform(
                  clip_schema_policy.time_conform_introduced_version) &&
              !clip_schema_policy.requires_fade_shape(
                  clip_schema_policy.fade_shape_introduced_version - 1) &&
              clip_schema_policy.requires_fade_shape(
                  clip_schema_policy.fade_shape_introduced_version));

} // namespace pulp::timeline::detail
