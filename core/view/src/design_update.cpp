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
    return old_node.type == new_node.type && old_node.render_mode == new_node.render_mode &&
           old_node.audio_widget == new_node.audio_widget &&
           binding_shape(old_node) == binding_shape(new_node);
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
        if (!shape_compatible(old_children[old_index], new_children[new_index])) {
            // A stable anchor identifies the source node, but does not prove
            // that its existing native view can host the new materialization.
            // Emit an explicit remove/insert pair so callers cannot reuse a
            // stale widget, render lane, or binding topology.
            append_update(plan, DesignUpdateKind::removed, key, old_index, 0);
            append_update(plan, DesignUpdateKind::inserted, key, 0, new_index);
        } else {
            append_update(
                plan, old_index == new_index ? DesignUpdateKind::retained : DesignUpdateKind::moved,
                key, old_index, new_index);
        }
    }
    for (std::size_t old_index = 0; old_index < old_children.size(); ++old_index) {
        if (!consumed[old_index])
            append_update(plan, DesignUpdateKind::removed, key_for(old_children[old_index]),
                          old_index, 0);
    }
    return plan;
}
} // namespace pulp::view
