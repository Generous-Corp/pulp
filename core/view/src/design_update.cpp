#include <algorithm>
#include <pulp/view/design_update.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
namespace pulp::view {
namespace {

const std::string& key_for(const IRNode& node) {
    static const std::string empty;
    return node.stable_anchor_id ? *node.stable_anchor_id : empty;
}

bool has_unique_nonempty_keys(std::span<const IRNode> nodes) {
    std::unordered_set<std::string> keys;
    keys.reserve(nodes.size());
    for (const auto& node : nodes) {
        const auto& key = key_for(node);
        if (key.empty() || !keys.insert(key).second)
            return false;
    }
    return true;
}

void append_update(DesignChildUpdatePlan& plan, DesignUpdateKind kind, std::string key,
                   std::size_t old_index, std::size_t new_index) {
    plan.updates.push_back({kind, std::move(key), old_index, new_index});
    const auto index = plan.updates.size() - 1;
    if (!plan.blocks.empty() && plan.blocks.back().kind == kind &&
        plan.blocks.back().first + plan.blocks.back().count == index) {
        ++plan.blocks.back().count;
    } else {
        plan.blocks.push_back({kind, index, 1});
    }
}
} // namespace

DesignChildUpdatePlan plan_design_child_updates(std::span<const IRNode> old_children,
                                                std::span<const IRNode> new_children) {
    DesignChildUpdatePlan plan;
    plan.keyed = has_unique_nonempty_keys(old_children) && has_unique_nonempty_keys(new_children);
    plan.ambiguous_keys = !plan.keyed;
    if (!plan.keyed) {
        // No identity is safe to retain. Replace the ambiguous sibling set
        // wholesale instead of accidentally reusing a view by position.
        for (std::size_t i = 0; i < old_children.size(); ++i)
            append_update(plan, DesignUpdateKind::removed, key_for(old_children[i]), i, 0);
        for (std::size_t i = 0; i < new_children.size(); ++i)
            append_update(plan, DesignUpdateKind::inserted, key_for(new_children[i]), 0, i);
        return plan;
    }
    std::unordered_map<std::string, std::size_t> old_by_key;
    old_by_key.reserve(old_children.size());
    for (std::size_t i = 0; i < old_children.size(); ++i)
        old_by_key.emplace(key_for(old_children[i]), i);
    std::vector<bool> consumed(old_children.size(), false);
    for (std::size_t new_index = 0; new_index < new_children.size(); ++new_index) {
        const auto& key = key_for(new_children[new_index]);
        const auto it = old_by_key.find(key);
        if (it == old_by_key.end()) {
            append_update(plan, DesignUpdateKind::inserted, key, 0, new_index);
            continue;
        }
        const auto old_index = it->second;
        consumed[old_index] = true;
        append_update(plan,
                      old_index == new_index ? DesignUpdateKind::retained : DesignUpdateKind::moved,
                      key, old_index, new_index);
    }
    for (std::size_t old_index = 0; old_index < old_children.size(); ++old_index) {
        if (!consumed[old_index])
            append_update(plan, DesignUpdateKind::removed, key_for(old_children[old_index]),
                          old_index, 0);
    }
    return plan;
}
} // namespace pulp::view
