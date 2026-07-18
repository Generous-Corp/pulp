#pragma once

#include "identity_directory.hpp"

namespace pulp::timeline::detail {

class ProjectStateAccess {
  public:
    static bool identities_equivalent(const Project& lhs, const Project& rhs) noexcept;
    static std::vector<IdentityRecord> identity_entries(const Project& project);
    static runtime::Result<Project, ModelError>
    restore_identities(Project project, std::vector<IdentityRecord> entries);
};

} // namespace pulp::timeline::detail
