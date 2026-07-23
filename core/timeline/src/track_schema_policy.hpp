#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct TrackSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
    std::uint32_t device_chain_introduced_version;
    std::uint32_t automation_introduced_version;
    std::uint32_t takes_introduced_version;

    [[nodiscard]] constexpr bool requires_device_chain(std::uint32_t version) const noexcept {
        return version >= device_chain_introduced_version;
    }

    [[nodiscard]] constexpr bool requires_automation(std::uint32_t version) const noexcept {
        return version >= automation_introduced_version;
    }

    // Take lanes and record-arm entered the schema together at v4.
    [[nodiscard]] constexpr bool requires_takes(std::uint32_t version) const noexcept {
        return version >= takes_introduced_version;
    }
};

inline constexpr TrackSchemaVersionPolicy track_schema_policy{
    "pulp.timeline.track",
    1,
    4,
    2,
    3,
    4,
};
static_assert(track_schema_policy.oldest_readable_version > 0 &&
              track_schema_policy.oldest_readable_version <=
                  track_schema_policy.current_version &&
              track_schema_policy.device_chain_introduced_version > 0 &&
              track_schema_policy.device_chain_introduced_version <=
                  track_schema_policy.current_version &&
              !track_schema_policy.requires_device_chain(
                  track_schema_policy.device_chain_introduced_version - 1) &&
              track_schema_policy.requires_device_chain(
                  track_schema_policy.device_chain_introduced_version) &&
              track_schema_policy.automation_introduced_version > 0 &&
              track_schema_policy.automation_introduced_version <=
                  track_schema_policy.current_version &&
              !track_schema_policy.requires_automation(
                  track_schema_policy.automation_introduced_version - 1) &&
              track_schema_policy.requires_automation(
                  track_schema_policy.automation_introduced_version) &&
              track_schema_policy.takes_introduced_version > 0 &&
              track_schema_policy.takes_introduced_version <=
                  track_schema_policy.current_version &&
              !track_schema_policy.requires_takes(
                  track_schema_policy.takes_introduced_version - 1) &&
              track_schema_policy.requires_takes(track_schema_policy.takes_introduced_version));

} // namespace pulp::timeline::detail
