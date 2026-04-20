#pragma once

// Internal hook exposed by permissions.cpp for platform backends. Not part
// of the public API — the override registry lives in permissions.cpp so the
// PermissionsOverride guard works on every platform, and backends call
// override_lookup() before dispatching a real OS query/prompt.

#include <pulp/platform/permissions.hpp>
#include <optional>

namespace pulp::platform::detail {

std::optional<PermissionState> override_lookup(Permission p);

}  // namespace pulp::platform::detail
