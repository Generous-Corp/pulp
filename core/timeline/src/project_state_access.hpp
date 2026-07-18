#pragma once

#include <pulp/timeline/model.hpp>

namespace pulp::timeline::detail {

class ProjectStateAccess {
  public:
    static bool identities_equivalent(const Project& lhs, const Project& rhs) noexcept;
};

} // namespace pulp::timeline::detail
