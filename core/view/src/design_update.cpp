#include <algorithm>
#include <cstdint>
#include <functional>
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

    // Alternate frames share the same DesignFrameView and participate in the
    // host-routing decision, so include their interactive topology too.  The
    // names remain deliberately omitted: those are re-keyable payload values.
    std::function<void(const IRNode&)> append_interactive = [&](const IRNode& frame) {
        for (const auto& element : frame.interactive_elements) {
            // Store the enum and binding presence, but never the parameter
            // name. The name is a re-keyable value; kind and binding
            // cardinality define the materialized control topology.
            shape.interactive.push_back(static_cast<std::uint8_t>(element.kind));
            shape.interactive.push_back(element.param_key.empty() ? 0 : 1);
        }
        for (const auto& alternate : frame.alternate_frames)
            append_interactive(alternate);
    };
    append_interactive(node);
    return shape;
}

// These fields are copied directly into DesignFrameElement by the native
// materializer.  A retained keyed child has no generic setter for them, so
// reusing its old view after one changes would leave the new IR pointing at a
// stale hit target, SVG patch, overlay, swap/action contract, or provenance
// record.  `param_key` is intentionally absent: DesignFrameView supports
// changing that binding in place via set_element_param_key().
bool interactive_materialization_equal(const IRInteractiveElement& old_element,
                                       const IRInteractiveElement& new_element) {
    return old_element.kind == new_element.kind && old_element.cx == new_element.cx &&
           old_element.cy == new_element.cy && old_element.hit_radius == new_element.hit_radius &&
           old_element.svg_patch_d == new_element.svg_patch_d &&
           old_element.default_value == new_element.default_value &&
           old_element.flash == new_element.flash && old_element.x == new_element.x &&
           old_element.y == new_element.y && old_element.w == new_element.w &&
           old_element.h == new_element.h && old_element.options == new_element.options &&
           old_element.selected_index == new_element.selected_index &&
           old_element.placeholder == new_element.placeholder &&
           old_element.bg_color == new_element.bg_color &&
           old_element.target_frame == new_element.target_frame &&
           old_element.action == new_element.action && old_element.text == new_element.text &&
           old_element.value_left_align == new_element.value_left_align &&
           old_element.default_value_y == new_element.default_value_y &&
           old_element.factory_id == new_element.factory_id &&
           old_element.custom_props == new_element.custom_props &&
           old_element.source_node_id.value_or("") == new_element.source_node_id.value_or("");
}

// Alternate frames are positional: a swap target refers to their index.  Keep
// their complete materialization identity in the same compatibility check as
// frame zero so an edit to an alternate cannot leave stale SVG/overlays behind.
bool frame_materialization_equal(const IRNode& old_frame, const IRNode& new_frame) {
    if (old_frame.render_mode != new_frame.render_mode ||
        old_frame.svg_asset_id != new_frame.svg_asset_id ||
        old_frame.capture_asset_id != new_frame.capture_asset_id ||
        old_frame.interactive_elements.size() != new_frame.interactive_elements.size() ||
        old_frame.alternate_frames.size() != new_frame.alternate_frames.size())
        return false;

    if (!std::equal(old_frame.interactive_elements.begin(), old_frame.interactive_elements.end(),
                    new_frame.interactive_elements.begin(), interactive_materialization_equal))
        return false;

    return std::equal(old_frame.alternate_frames.begin(), old_frame.alternate_frames.end(),
                      new_frame.alternate_frames.begin(), frame_materialization_equal);
}

bool shape_compatible(const IRNode& old_node, const IRNode& new_node) {
    if (old_node.type != new_node.type || old_node.render_mode != new_node.render_mode ||
        old_node.audio_widget != new_node.audio_widget ||
        old_node.svg_asset_id != new_node.svg_asset_id ||
        old_node.capture_asset_id != new_node.capture_asset_id ||
        old_node.interactive_elements.size() != new_node.interactive_elements.size() ||
        binding_shape(old_node) != binding_shape(new_node))
        return false;

    return frame_materialization_equal(old_node, new_node);
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
