#pragma once

#include <pulp/timeline/smf.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace pulp::timeline::detail {

// Shared failure construction so every SMF error names its offending construct
// instead of returning a bare code.
inline SmfError smf_error(SmfErrorCode code, std::string message) {
    return SmfError{code, std::move(message), ModelError{}};
}

inline std::string decimal(std::int64_t value) {
    return std::to_string(value);
}

} // namespace pulp::timeline::detail
