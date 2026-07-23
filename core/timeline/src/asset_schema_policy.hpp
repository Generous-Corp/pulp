#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct AssetSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
    std::uint32_t loop_info_introduced_version;

    [[nodiscard]] constexpr bool supports_loop_info(std::uint32_t version) const noexcept {
        return version >= loop_info_introduced_version;
    }
};

inline constexpr AssetSchemaVersionPolicy asset_schema_policy{
    "pulp.timeline.asset",
    1,
    2,
    2,
};

static_assert(asset_schema_policy.oldest_readable_version > 0 &&
              asset_schema_policy.oldest_readable_version <=
                  asset_schema_policy.current_version &&
              asset_schema_policy.loop_info_introduced_version > 0 &&
              asset_schema_policy.loop_info_introduced_version <=
                  asset_schema_policy.current_version &&
              !asset_schema_policy.supports_loop_info(
                  asset_schema_policy.loop_info_introduced_version - 1) &&
              asset_schema_policy.supports_loop_info(
                  asset_schema_policy.loop_info_introduced_version));

} // namespace pulp::timeline::detail
