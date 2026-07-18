#pragma once

#include <pulp/timeline/model.hpp>

#include <memory>
#include <optional>

namespace pulp::timeline::detail {

struct IdentityNode;

// Private persistent index for project-wide identity ownership. Copies are
// cheap; insert/update path-copy only an AVL search path.
class IdentityDirectory {
  public:
    bool insert(ItemId id, ItemLocation location);
    bool replace(ItemId id, ItemLocation location);
    std::optional<ItemLocation> locate(ItemId id) const noexcept;
    bool equivalent(const IdentityDirectory& other) const noexcept;
    std::size_t shared_nodes_with(const IdentityDirectory& other) const;
    static ProjectIdentityStats stats() noexcept;

  private:
    std::shared_ptr<const IdentityNode> root_;
};

} // namespace pulp::timeline::detail
