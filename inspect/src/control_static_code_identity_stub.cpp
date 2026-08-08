#include "control_static_code_identity.hpp"

namespace pulp::inspect::detail {

std::optional<ControlTrustedHostStaticExpectation>
inspect_static_code_identity(const std::filesystem::path&) {
    return std::nullopt;
}

} // namespace pulp::inspect::detail
