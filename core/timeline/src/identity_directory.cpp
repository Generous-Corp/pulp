#include "identity_directory.hpp"

#include <algorithm>
#include <atomic>
#include <unordered_set>

namespace pulp::timeline::detail {

static std::atomic<std::uint64_t> g_live_nodes{0};
static std::atomic<std::uint64_t> g_created_nodes{0};

struct IdentityNode {
    ItemId id;
    ItemLocation location;
    std::shared_ptr<const IdentityNode> left;
    std::shared_ptr<const IdentityNode> right;
    std::uint8_t height = 1;
    std::size_t count = 1;

    IdentityNode(ItemId value, ItemLocation item_location, std::shared_ptr<const IdentityNode> lhs,
                 std::shared_ptr<const IdentityNode> rhs, std::uint8_t node_height,
                 std::size_t node_count)
        : id(value), location(item_location), left(std::move(lhs)), right(std::move(rhs)),
          height(node_height), count(node_count) {
        ++g_live_nodes;
        ++g_created_nodes;
    }
    ~IdentityNode() {
        --g_live_nodes;
    }
};

namespace {

using NodePtr = std::shared_ptr<const IdentityNode>;

std::uint8_t height(const NodePtr& value) noexcept {
    return value ? value->height : 0;
}
std::size_t count(const NodePtr& value) noexcept {
    return value ? value->count : 0;
}

NodePtr node(ItemId id, ItemLocation location, NodePtr left = {}, NodePtr right = {}) {
    const auto node_height = static_cast<std::uint8_t>(1 + std::max(height(left), height(right)));
    const auto node_count = 1 + count(left) + count(right);
    return std::make_shared<const IdentityNode>(id, location, std::move(left), std::move(right),
                                                node_height, node_count);
}

NodePtr balance(ItemId id, ItemLocation location, NodePtr left, NodePtr right) {
    if (height(left) > height(right) + 1) {
        if (height(left->left) < height(left->right)) {
            const auto pivot = left->right;
            auto rotated = node(left->id, left->location, left->left, pivot->left);
            left = node(pivot->id, pivot->location, std::move(rotated), pivot->right);
        }
        const auto pivot = left;
        return node(pivot->id, pivot->location, pivot->left,
                    node(id, location, pivot->right, std::move(right)));
    }
    if (height(right) > height(left) + 1) {
        if (height(right->right) < height(right->left)) {
            const auto pivot = right->left;
            auto rotated = node(right->id, right->location, pivot->right, right->right);
            right = node(pivot->id, pivot->location, pivot->left, std::move(rotated));
        }
        const auto pivot = right;
        return node(pivot->id, pivot->location, node(id, location, std::move(left), pivot->left),
                    pivot->right);
    }
    return node(id, location, std::move(left), std::move(right));
}

NodePtr insert(NodePtr root, ItemId id, ItemLocation location, bool& duplicate) {
    if (!root)
        return node(id, location);
    if (id == root->id) {
        duplicate = true;
        return root;
    }
    if (id < root->id)
        return balance(root->id, root->location, insert(root->left, id, location, duplicate),
                       root->right);
    return balance(root->id, root->location, root->left,
                   insert(root->right, id, location, duplicate));
}

NodePtr replace(NodePtr root, ItemId id, ItemLocation location, bool& found) {
    if (!root)
        return {};
    if (id == root->id) {
        found = true;
        return node(id, location, root->left, root->right);
    }
    if (id < root->id) {
        auto changed = replace(root->left, id, location, found);
        return found ? node(root->id, root->location, std::move(changed), root->right) : root;
    }
    auto changed = replace(root->right, id, location, found);
    return found ? node(root->id, root->location, root->left, std::move(changed)) : root;
}

const IdentityNode* find(const NodePtr& root, ItemId id) noexcept {
    auto* current = root.get();
    while (current) {
        if (id == current->id)
            return current;
        current = id < current->id ? current->left.get() : current->right.get();
    }
    return nullptr;
}

bool same_location(const ItemLocation& lhs, const ItemLocation& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.sequence_id == rhs.sequence_id &&
           lhs.track_id == rhs.track_id && lhs.clip_id == rhs.clip_id &&
           lhs.automation_lane_id == rhs.automation_lane_id && lhs.active == rhs.active;
}

bool entries_match(const NodePtr& source, const NodePtr& other) noexcept {
    if (!source)
        return true;
    const auto* match = find(other, source->id);
    return match && same_location(source->location, match->location) &&
           entries_match(source->left, other) && entries_match(source->right, other);
}

void collect(const NodePtr& root, std::unordered_set<const IdentityNode*>& addresses) {
    if (!root)
        return;
    addresses.insert(root.get());
    collect(root->left, addresses);
    collect(root->right, addresses);
}

void collect_entries(const NodePtr& root, std::vector<IdentityRecord>& entries) {
    if (!root)
        return;
    collect_entries(root->left, entries);
    entries.push_back({root->id, root->location});
    collect_entries(root->right, entries);
}

std::size_t shared(const NodePtr& root, const std::unordered_set<const IdentityNode*>& addresses) {
    if (!root)
        return 0;
    if (addresses.contains(root.get()))
        return root->count;
    return shared(root->left, addresses) + shared(root->right, addresses);
}

} // namespace

bool IdentityDirectory::insert(ItemId id, ItemLocation location) {
    bool duplicate = false;
    root_ = detail::insert(std::move(root_), id, location, duplicate);
    return !duplicate;
}

bool IdentityDirectory::replace(ItemId id, ItemLocation location) {
    bool found = false;
    root_ = detail::replace(std::move(root_), id, location, found);
    return found;
}

std::optional<ItemLocation> IdentityDirectory::locate(ItemId id) const noexcept {
    const auto* found = detail::find(root_, id);
    return found ? std::optional<ItemLocation>(found->location) : std::nullopt;
}

bool IdentityDirectory::equivalent(const IdentityDirectory& other) const noexcept {
    return count(root_) == count(other.root_) && entries_match(root_, other.root_);
}

std::vector<IdentityRecord> IdentityDirectory::entries() const {
    std::vector<IdentityRecord> result;
    result.reserve(count(root_));
    collect_entries(root_, result);
    return result;
}

std::size_t IdentityDirectory::shared_nodes_with(const IdentityDirectory& other) const {
    std::unordered_set<const IdentityNode*> addresses;
    collect(root_, addresses);
    return shared(other.root_, addresses);
}

ProjectIdentityStats IdentityDirectory::stats() noexcept {
    return {g_live_nodes.load(), g_created_nodes.load()};
}

} // namespace pulp::timeline::detail
