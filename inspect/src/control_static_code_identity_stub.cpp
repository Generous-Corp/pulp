#include "control_static_code_identity.hpp"

namespace pulp::inspect::detail {

std::optional<ControlTrustedHostStaticExpectation>
inspect_static_code_identity(const std::filesystem::path&) {
    return std::nullopt;
}

bool is_apple_platform_code(const std::filesystem::path&) {
    return false;
}

} // namespace pulp::inspect::detail
