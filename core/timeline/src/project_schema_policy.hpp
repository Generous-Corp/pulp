#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::timeline::detail {

struct ProjectSchemaVersionPolicy {
    std::string_view type_name;
    std::uint32_t oldest_readable_version;
    std::uint32_t current_version;
    std::uint32_t session_start_introduced_version;
    std::uint32_t tuning_introduced_version;

    // The session origin is an optional member: a payload at or above its
    // introducing version may carry it, and an older payload may not.
    [[nodiscard]] constexpr bool supports_session_start(std::uint32_t version) const noexcept {
        return version >= session_start_introduced_version;
    }

    // The tuning is an optional member on the same terms as the session origin.
    [[nodiscard]] constexpr bool supports_tuning(std::uint32_t version) const noexcept {
        return version >= tuning_introduced_version;
    }
};

inline constexpr ProjectSchemaVersionPolicy project_schema_policy{
    "pulp.timeline.project",
    1,
    3,
    2,
    3,
};
static_assert(project_schema_policy.oldest_readable_version > 0 &&
              project_schema_policy.oldest_readable_version <=
                  project_schema_policy.current_version &&
              project_schema_policy.session_start_introduced_version > 0 &&
              project_schema_policy.session_start_introduced_version <=
                  project_schema_policy.current_version &&
              !project_schema_policy.supports_session_start(
                  project_schema_policy.session_start_introduced_version - 1) &&
              project_schema_policy.supports_session_start(
                  project_schema_policy.session_start_introduced_version));
static_assert(project_schema_policy.tuning_introduced_version >
                  project_schema_policy.session_start_introduced_version &&
              project_schema_policy.tuning_introduced_version <=
                  project_schema_policy.current_version &&
              !project_schema_policy.supports_tuning(
                  project_schema_policy.tuning_introduced_version - 1) &&
              project_schema_policy.supports_tuning(
                  project_schema_policy.tuning_introduced_version));

} // namespace pulp::timeline::detail
