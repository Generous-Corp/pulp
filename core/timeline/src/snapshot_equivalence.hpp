#pragma once

#include <pulp/timeline/model.hpp>

namespace pulp::timeline::detail {

bool snapshots_equivalent(const Project& lhs, const Project& rhs) noexcept;

} // namespace pulp::timeline::detail
