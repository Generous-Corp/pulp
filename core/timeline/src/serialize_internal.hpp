#pragma once

#include <pulp/timeline/schema_registry.hpp>

#include <optional>

namespace pulp::timeline::detail {

std::optional<PersistenceErrorCode>
validate_structural_registry(const SchemaRegistry& registry) noexcept;

} // namespace pulp::timeline::detail
