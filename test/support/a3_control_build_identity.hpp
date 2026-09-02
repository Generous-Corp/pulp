#pragma once

#include <string_view>

#if !defined(PULP_A3_CONTROL_TARGET) || !defined(PULP_A3_CONTROL_SOURCE_PATH) ||            \
    !defined(PULP_A3_CONTROL_SOURCE_REVISION) || !defined(PULP_A3_CONTROL_SOURCE_BLOB) ||  \
    !defined(PULP_A3_CONTROL_BUILD_ID) || !defined(PULP_A3_CONTROL_BUILD_CONFIG) ||        \
    !defined(PULP_A3_CONTROL_CONFIGURED_AT_UTC) || !defined(PULP_A3_CONTROL_GIT_DIRTY)
#error "A3 control targets require an exact CMake build identity"
#endif

#define PULP_A3_CONTROL_BUILD_IDENTITY_JSON                                               \
    "{\"schema\":\"pulp.gpu-first-visible-control-build-identity.v1\",\"version\":1," \
    "\"target\":\"" PULP_A3_CONTROL_TARGET "\",\"source_path\":\""                    \
    PULP_A3_CONTROL_SOURCE_PATH "\",\"source_revision\":\""                            \
    PULP_A3_CONTROL_SOURCE_REVISION "\",\"source_blob\":\""                            \
    PULP_A3_CONTROL_SOURCE_BLOB "\",\"build_id\":\"" PULP_A3_CONTROL_BUILD_ID          \
    "\",\"build_config\":\"" PULP_A3_CONTROL_BUILD_CONFIG                             \
    "\",\"configured_at_utc\":\"" PULP_A3_CONTROL_CONFIGURED_AT_UTC                   \
    "\",\"git_dirty\":" PULP_A3_CONTROL_GIT_DIRTY "}"

namespace pulp::test {

inline constexpr std::string_view kA3ControlBuildIdentityJson =
    PULP_A3_CONTROL_BUILD_IDENTITY_JSON;

#if defined(__GNUC__) || defined(__clang__)
[[gnu::used]]
#endif
inline constexpr char kA3ControlBuildIdentityMarker[] =
    "\0PULP_A3_CONTROL_BUILD_IDENTITY_V1:" PULP_A3_CONTROL_BUILD_IDENTITY_JSON
    ":END_PULP_A3_CONTROL_BUILD_IDENTITY\0";

} // namespace pulp::test

#undef PULP_A3_CONTROL_BUILD_IDENTITY_JSON
