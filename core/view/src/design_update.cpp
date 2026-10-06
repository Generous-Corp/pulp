#include <algorithm>
#include <cstdint>
#include <pulp/view/design_update.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
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

bool has_nonempty_attribute(const IRNode& node, std::string_view key) {
    const auto it = node.attributes.find(std::string(key));
    return it != node.attributes.end() && !it->second.empty();
}

// Binding names are values that can be re-keyed on an existing control. The
// booleans below intentionally record only the binding *shape*: changing a
// scalar key from "gain" to "cutoff" must not discard the control, while
// changing scalar↔XY, bound↔unbound, or adding a native overlay must.
struct BindingShape {
    bool scalar = false;
    bool x = false;
    bool y = false;
    bool meter = false;
    bool value = false;
    bool action = false;
    std::vector<std::uint8_t> interactive;

    friend bool operator==(const BindingShape&, const BindingShape&) = default;
};

BindingShape binding_shape(const IRNode& node) {
    BindingShape shape;
    shape.scalar = has_nonempty_attribute(node, "binding") ||
                   has_nonempty_attribute(node, "pulpParamKey") ||
                   has_nonempty_attribute(node, "pulpBindingModule") ||
                   has_nonempty_attribute(node, "pulpBindingParam");
    shape.x = has_nonempty_attribute(node, "pulpParamKeyX") ||
              has_nonempty_attribute(node, "pulpBindingModuleX") ||
              has_nonempty_attribute(node, "pulpBindingParamX");
    shape.y = has_nonempty_attribute(node, "pulpParamKeyY") ||
              has_nonempty_attribute(node, "pulpBindingModuleY") ||
              has_nonempty_attribute(node, "pulpBindingParamY");
    shape.meter = has_nonempty_attribute(node, "pulpMeterSource") ||
                  has_nonempty_attribute(node, "pulpMeterChannel") ||
                  has_nonempty_attribute(node, "pulpMeterValueKey");
    // The value key selects the binding family. Initial text and placeholder
    // are payload values on that family, so changing either must remain an
    // in-place update rather than recreating the native control.
    shape.value = has_nonempty_attribute(node, "pulpValueKey");
    shape.action = has_nonempty_attribute(node, "pulpHostAction") ||
                   has_nonempty_attribute(node, "pulpPayloadContract");

    shape.interactive.reserve(node.interactive_elements.size() * 2);
    for (const auto& element : node.interactive_elements) {
        // Store the enum and binding presence, but never the parameter name.
        // The name is a re-keyable value; kind and binding cardinality define
        // the materialized control topology.
        shape.interactive.push_back(static_cast<std::uint8_t>(element.kind));
        shape.interactive.push_back(element.param_key.empty() ? 0 : 1);
    }
    return shape;
}

bool shape_compatible(const IRNode& old_node, const IRNode& new_node) {
    if (old_node.type != new_node.type || old_node.render_mode != new_node.render_mode ||
        old_node.audio_widget != new_node.audio_widget ||
        old_node.svg_asset_id != new_node.svg_asset_id ||
        old_node.capture_asset_id != new_node.capture_asset_id ||
        old_node.interactive_elements.size() != new_node.interactive_elements.size() ||
        binding_shape(old_node) != binding_shape(new_node))
        return false;

    // These fields select the native materialization behind a stable anchor.
    // Reusing the old view after one changes leaves the new node pointing at
    // stale SVG/capture bytes or at the wrong custom-control factory/config.
    return std::equal(old_node.interactive_elements.begin(), old_node.interactive_elements.end(),
                      new_node.interactive_elements.begin(),
                      [](const auto& old_element, const auto& new_element) {
                          return old_element.factory_id == new_element.factory_id &&
                                 old_element.custom_props == new_element.custom_props;
                      });
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
        // Removals are emitted from the end so these indices remain valid for
        // a materializer that applies the plan directly to its child vector.
        for (std::size_t i = old_children.size(); i-- > 0;)
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
    std::vector<bool> recreate(new_children.size(), false);
    std::vector<bool> recreate_by_old(old_children.size(), false);

    // First classify the old materializations.  All removals must precede
    // moves/inserts: otherwise an index-based consumer can move a child and
    // then remove that same slot, deleting the wrong identity.
    for (std::size_t new_index = 0; new_index < new_children.size(); ++new_index) {
        const auto& key = key_for(new_children[new_index]);
        const auto it = old_by_key.find(key);
        if (it == old_by_key.end())
            continue;
        const auto old_index = it->second;
        consumed[old_index] = true;
        recreate[new_index] = !shape_compatible(old_children[old_index], new_children[new_index]);
        recreate_by_old[old_index] = recreate[new_index];
    }

    for (std::size_t old_index = old_children.size(); old_index-- > 0;) {
        if (!consumed[old_index] || recreate_by_old[old_index])
            append_update(plan, DesignUpdateKind::removed, key_for(old_children[old_index]),
                          old_index, 0);
    }

    // Track the post-removal working order so every subsequent index is
    // directly applicable to a mutable child vector.
    std::vector<std::string> working;
    working.reserve(old_children.size() + new_children.size());
    for (std::size_t old_index = 0; old_index < old_children.size(); ++old_index) {
        if (consumed[old_index] && !recreate_by_old[old_index])
            working.push_back(key_for(old_children[old_index]));
    }
    for (std::size_t new_index = 0; new_index < new_children.size(); ++new_index) {
        const auto& key = key_for(new_children[new_index]);
        const auto it = old_by_key.find(key);
        if (it == old_by_key.end() || recreate[new_index]) {
            append_update(plan, DesignUpdateKind::inserted, key, 0, new_index);
            working.insert(working.begin() + static_cast<std::ptrdiff_t>(new_index), key);
            continue;
        }
        const auto current = std::find(working.begin(), working.end(), key);
        const auto old_index = static_cast<std::size_t>(current - working.begin());
        if (old_index == new_index) {
            append_update(plan, DesignUpdateKind::retained, key, old_index, new_index);
        } else {
            append_update(plan, DesignUpdateKind::moved, key, old_index, new_index);
            auto value = *current;
            working.erase(current);
            working.insert(working.begin() + static_cast<std::ptrdiff_t>(new_index),
                           std::move(value));
        }
    }
    return plan;
}
} // namespace pulp::view
