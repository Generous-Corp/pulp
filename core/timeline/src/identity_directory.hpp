#pragma once

#include <pulp/timeline/model.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline::detail {

struct IdentityNode;

struct IdentityRecord {
    ItemId item;
    ItemLocation location;
};

// Private persistent index for project-wide identity ownership. Copies are
// cheap; insert/update path-copy only an AVL search path.
class IdentityDirectory {
  public:
    static IdentityDirectory from_sorted_entries(std::span<const IdentityRecord> entries);
    bool insert(ItemId id, ItemLocation location);
    bool replace(ItemId id, ItemLocation location);
    std::optional<ItemLocation> locate(ItemId id) const noexcept;
    bool equivalent(const IdentityDirectory& other) const noexcept;
    std::vector<IdentityRecord> entries() const;
    std::size_t shared_nodes_with(const IdentityDirectory& other) const;
    static ProjectIdentityStats stats() noexcept;

  private:
    std::shared_ptr<const IdentityNode> root_;
};

} // namespace pulp::timeline::detail
