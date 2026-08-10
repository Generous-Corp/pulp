#pragma once

#include <pulp/inspect/control_trusted_host_inventory.hpp>

#include <filesystem>
#include <optional>

namespace pulp::inspect::detail {

std::optional<ControlTrustedHostStaticExpectation>
inspect_static_code_identity(const std::filesystem::path& executable);
bool is_apple_platform_code(const std::filesystem::path& executable);

} // namespace pulp::inspect::detail
