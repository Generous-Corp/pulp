#pragma once

#include <pulp/timeline/schema_json.hpp>

#include <optional>
#include <string_view>

namespace pulp::timeline::detail {

std::optional<PersistenceError> validate_json_syntax_and_limits(std::string_view source,
                                                                const DecodeLimits& limits);

} // namespace pulp::timeline::detail
