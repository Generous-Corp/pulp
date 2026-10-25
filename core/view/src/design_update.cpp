#include <algorithm>
#include <cstdint>
#include <pulp/view/design_update.hpp>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
namespace pulp::view {
namespace {

struct SlotAttributeView {
    std::string_view route;
    std::string_view binding;
    std::string_view parameter;
    std::string_view module;
    std::string_view module_parameter;
    std::string_view x;
    std::string_view y;
    std::string_view x_module;
    std::string_view x_parameter;
    std::string_view y_module;
    std::string_view y_parameter;
    std::string_view meter;
    std::string_view meter_source;
    std::string_view meter_channel;
    std::string_view text;
    std::string_view action;
};

SlotAttributeView slot_attributes(const IRNode& node) {
    SlotAttributeView view;
    for (const auto& [key, value] : node.attributes) {
        if (key == "pulpRouteId")
            view.route = value;
        else if (key == "binding")
            view.binding = value;
        else if (key == "pulpParamKey")
            view.parameter = value;
        else if (key == "pulpBindingModule")
            view.module = value;
        else if (key == "pulpBindingParam")
            view.module_parameter = value;
        else if (key == "pulpParamKeyX")
            view.x = value;
        else if (key == "pulpParamKeyY")
            view.y = value;
        else if (key == "pulpBindingModuleX")
            view.x_module = value;
        else if (key == "pulpBindingParamX")
            view.x_parameter = value;
        else if (key == "pulpBindingModuleY")
            view.y_module = value;
        else if (key == "pulpBindingParamY")
            view.y_parameter = value;
        else if (key == "pulpMeterValueKey")
            view.meter = value;
        else if (key == "pulpMeterSource")
            view.meter_source = value;
        else if (key == "pulpMeterChannel")
            view.meter_channel = value;
        else if (key == "pulpValueKey")
            view.text = value;
        else if (key == "pulpHostAction")
            view.action = value;
    }
    return view;
}

void append_slot(std::vector<DesignSlot>& slots, DesignSlotKind kind, std::string_view route,
                 std::string_view key) {
    slots.push_back({kind, std::string(route), std::string(key)});
}

std::string encode_vector_key(std::string_view x, std::string_view y) {
    return std::to_string(x.size()) + ":" + std::string(x) + std::to_string(y.size()) + ":" +
           std::string(y);
}

std::string encode_pair_key(std::string_view first, std::string_view second) {
    return std::to_string(first.size()) + ":" + std::string(first) + std::to_string(second.size()) +
           ":" + std::string(second);
}

DesignSlotKind interactive_slot_kind(InteractiveElementKind kind) {
    switch (kind) {
    case InteractiveElementKind::text_field:
    case InteractiveElementKind::value_label:
        return DesignSlotKind::dynamic_text;
    case InteractiveElementKind::xy_pad:
        return DesignSlotKind::dynamic_vector;
    default:
        return DesignSlotKind::dynamic_scalar;
    }
}

void append_node_slots(const IRNode& node, std::vector<DesignSlot>& slots) {
    const auto attributes = slot_attributes(node);
    const auto route = attributes.route;
    bool has_dynamic_slot = false;
    std::string scalar;
    const auto parameter = attributes.parameter;
    const bool has_vector_binding = !attributes.x.empty() || !attributes.y.empty() ||
                                    !attributes.x_module.empty() ||
                                    !attributes.x_parameter.empty() ||
                                    !attributes.y_module.empty() || !attributes.y_parameter.empty();
    const bool has_meter_binding = !attributes.meter.empty() || !attributes.meter_source.empty() ||
                                   !attributes.meter_channel.empty();
    if (!parameter.empty())
        scalar = parameter;
    else if (!attributes.module.empty() || !attributes.module_parameter.empty()) {
        const auto module = attributes.module;
        const auto param = attributes.module_parameter;
        scalar = encode_pair_key(module, param);
    } else if (!has_vector_binding && !has_meter_binding && !attributes.binding.empty()) {
        scalar = std::string(attributes.binding);
    }
    const auto x = attributes.x.empty()
                       ? encode_pair_key(attributes.x_module, attributes.x_parameter)
                       : std::string(attributes.x);
    const auto y = attributes.y.empty()
                       ? encode_pair_key(attributes.y_module, attributes.y_parameter)
                       : std::string(attributes.y);
    if (!scalar.empty()) {
        append_slot(slots, DesignSlotKind::dynamic_scalar, route, scalar);
        has_dynamic_slot = true;
    }
    if (has_vector_binding) {
        append_slot(slots, DesignSlotKind::dynamic_vector, route, encode_vector_key(x, y));
        has_dynamic_slot = true;
    }

    const auto meter = attributes.meter;
    const auto meter_source = attributes.meter_source;
    const auto meter_channel = attributes.meter_channel;
    if (!meter.empty() || !meter_source.empty() || !meter_channel.empty()) {
        std::string meter_key;
        if (!meter.empty())
            meter_key = meter;
        else
            meter_key = encode_pair_key(meter_source, meter_channel);
        append_slot(slots, DesignSlotKind::dynamic_meter, route, meter_key);
        has_dynamic_slot = true;
    }

    const auto text = attributes.text;
    if (!text.empty()) {
        append_slot(slots, DesignSlotKind::dynamic_text, route, text);
        has_dynamic_slot = true;
    }

    const auto action = attributes.action;
    if (!action.empty()) {
        append_slot(slots, DesignSlotKind::dynamic_action, route, action);
        has_dynamic_slot = true;
    }

    // Geometry-detected controls carry their host route on the interactive
    // element instead of the node-level pulp* attributes. Preserve the
    // element's typed setter lane in the same inventory used by recognized
    // widgets; this is what lets a re-key update avoid rebuilding its SVG
    // record.
    for (const auto& element : node.interactive_elements) {
        if (!element.param_key.empty()) {
            append_slot(slots, interactive_slot_kind(element.kind), route, element.param_key);
            has_dynamic_slot = true;
        }
        if (!element.action.empty()) {
            append_slot(slots, DesignSlotKind::dynamic_action, route, element.action);
            has_dynamic_slot = true;
        }
    }

    if (!has_dynamic_slot)
        append_slot(slots, DesignSlotKind::static_visual, route, {});

    for (const auto& child : node.children)
        append_node_slots(child, slots);
    for (const auto& frame : node.alternate_frames)
        append_node_slots(frame, slots);
}

int slot_kind_rank(DesignSlotKind kind) {
    return static_cast<int>(kind);
}

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
    for (const auto& [attribute, value] : node.attributes) {
        if (attribute == key)
            return !value.empty();
    }
    return false;
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
    // Contract fields are copied into native binding callbacks or the initial
    // widget configuration. A retained node has no generic setter for them,
    // so compare their values rather than only their presence. Re-keyable
    // names and text payloads are intentionally excluded below.
    std::vector<std::pair<std::string, std::string>> contract;

    friend bool operator==(const BindingShape&, const BindingShape&) = default;
};

bool is_rekeyable_binding_attribute(std::string_view key) {
    // These are names/payloads that an update consumer can change in place.
    // Binding module/param values are deliberately absent: NativeImportBindingContext
    // consumes them when installing the callback and has no re-key setter. Retaining
    // a view across either change would leave the stable anchor pointing at the old route.
    // The planner must still compare their presence through the topology bits
    // above so bound↔unbound transitions recreate the native control.
    for (const auto candidate :
         {"pulpParamKey", "pulpParamKeyX", "pulpParamKeyY", "pulpMeterSource", "pulpMeterChannel",
          "pulpMeterValueKey", "pulpValueKey", "pulpInitialValue", "pulpPlaceholder"}) {
        if (key == candidate)
            return true;
    }
    return false;
}

void append_binding_shape(const IRNode& frame, BindingShape& shape) {
    shape.scalar = shape.scalar || has_nonempty_attribute(frame, "binding") ||
                   has_nonempty_attribute(frame, "pulpParamKey") ||
                   has_nonempty_attribute(frame, "pulpBindingModule") ||
                   has_nonempty_attribute(frame, "pulpBindingParam");
    shape.x = shape.x || has_nonempty_attribute(frame, "pulpParamKeyX") ||
              has_nonempty_attribute(frame, "pulpBindingModuleX") ||
              has_nonempty_attribute(frame, "pulpBindingParamX");
    shape.y = shape.y || has_nonempty_attribute(frame, "pulpParamKeyY") ||
              has_nonempty_attribute(frame, "pulpBindingModuleY") ||
              has_nonempty_attribute(frame, "pulpBindingParamY");
    shape.meter = shape.meter || has_nonempty_attribute(frame, "pulpMeterSource") ||
                  has_nonempty_attribute(frame, "pulpMeterChannel") ||
                  has_nonempty_attribute(frame, "pulpMeterValueKey");
    // The value key selects the binding family. Initial text and placeholder
    // are payload values on that family, so changing either must remain an
    // in-place update rather than recreating the native control.
    shape.value = shape.value || has_nonempty_attribute(frame, "pulpValueKey");
    shape.action = shape.action || has_nonempty_attribute(frame, "pulpHostAction") ||
                   has_nonempty_attribute(frame, "pulpPayloadContract");

    for (const auto& element : frame.interactive_elements) {
        // Store the enum and binding presence, but never the parameter name.
        // The name is a re-keyable value; kind and binding cardinality define
        // the materialized control topology.
        shape.interactive.push_back(static_cast<std::uint8_t>(element.kind));
        shape.interactive.push_back(element.param_key.empty() ? 0 : 1);
    }

    // Native binding and widget-contract attributes are immutable inputs to
    // the materializer. Keep exact values, sorted by key for deterministic
    // comparison despite unordered IR attribute storage. The re-keyable
    // names/text payloads above are deliberately omitted.
    for (const auto& [key, value] : frame.attributes) {
        if (key.rfind("pulp", 0) == 0 && !is_rekeyable_binding_attribute(key))
            shape.contract.emplace_back(key, value);
    }

    for (const auto& alternate : frame.alternate_frames)
        append_binding_shape(alternate, shape);
}

BindingShape binding_shape(const IRNode& node) {
    BindingShape shape;
    append_binding_shape(node, shape);
    std::sort(shape.contract.begin(), shape.contract.end());
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
    if (old_frame.type != new_frame.type || old_frame.audio_widget != new_frame.audio_widget ||
        old_frame.render_mode != new_frame.render_mode ||
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
    switch (kind) {
    case DesignUpdateKind::retained:
        ++plan.work.retained;
        break;
    case DesignUpdateKind::moved:
        ++plan.work.moved;
        break;
    case DesignUpdateKind::inserted:
        ++plan.work.inserted;
        break;
    case DesignUpdateKind::removed:
        ++plan.work.removed;
        break;
    }
    const auto index = plan.updates.size() - 1;
    if (!plan.blocks.empty() && plan.blocks.back().kind == kind &&
        plan.blocks.back().first + plan.blocks.back().count == index) {
        ++plan.blocks.back().count;
    } else {
        plan.blocks.push_back({kind, index, 1});
    }
}
void append_key_diagnostics(std::span<const IRNode> nodes, bool old_side,
                            std::vector<DesignKeyDiagnostic>& out) {
    const bool any_keyed = std::any_of(nodes.begin(), nodes.end(),
                                       [](const IRNode& node) { return !key_for(node).empty(); });
    if (!any_keyed) {
        if (!nodes.empty())
            out.push_back({DesignKeyDiagnosticReason::missing, {}, 0, old_side});
        return;
    }
    std::unordered_set<std::string_view> seen;
    seen.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& key = key_for(nodes[i]);
        if (key.empty())
            out.push_back({DesignKeyDiagnosticReason::mixed, {}, i, old_side});
        else if (!seen.insert(key).second)
            out.push_back({DesignKeyDiagnosticReason::duplicate, key, i, old_side});
    }
}

// Indices into `sequence` of one longest strictly increasing subsequence,
// in ascending order. Patience sorting keeps, for each length, the entry
// with the smallest tail, so ties resolve toward earlier old positions.
std::vector<std::size_t> longest_increasing_subsequence(std::span<const std::size_t> sequence) {
    std::vector<std::size_t> tails;
    std::vector<std::size_t> previous(sequence.size(), sequence.size());
    tails.reserve(sequence.size());
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        const auto slot = std::lower_bound(
            tails.begin(), tails.end(), sequence[i],
            [&](std::size_t tail, std::size_t value) { return sequence[tail] < value; });
        if (slot != tails.begin())
            previous[i] = *(slot - 1);
        if (slot == tails.end())
            tails.push_back(i);
        else
            *slot = i;
    }
    std::vector<std::size_t> result(tails.size());
    for (std::size_t at = tails.empty() ? sequence.size() : tails.back(), n = result.size();
         n-- > 0; at = previous[at])
        result[n] = at;
    return result;
}

// Counts live children per slot so a slot's current child-vector position
// is a prefix sum.
class SlotPositions {
  public:
    explicit SlotPositions(std::size_t size) : tree_(size + 1, 0) {}

    void add(std::size_t slot, int delta) {
        for (auto i = slot + 1; i < tree_.size(); i += i & (~i + 1))
            tree_[i] += delta;
    }

    std::size_t before(std::size_t slot) const {
        int count = 0;
        for (auto i = slot; i > 0; i -= i & (~i + 1))
            count += tree_[i];
        return static_cast<std::size_t>(count);
    }

  private:
    std::vector<int> tree_;
};
} // namespace

DesignChildUpdatePlan plan_design_child_updates(std::span<const IRNode> old_children,
                                                std::span<const IRNode> new_children) {
    DesignChildUpdatePlan plan;
    plan.keyed = has_unique_nonempty_keys(old_children) && has_unique_nonempty_keys(new_children);
    plan.ambiguous_keys = !plan.keyed;
    if (!plan.keyed) {
        append_key_diagnostics(old_children, true, plan.key_diagnostics);
        append_key_diagnostics(new_children, false, plan.key_diagnostics);
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
    constexpr auto none = static_cast<std::size_t>(-1);
    std::unordered_map<std::string_view, std::size_t> old_by_key;
    old_by_key.reserve(old_children.size());
    for (std::size_t i = 0; i < old_children.size(); ++i)
        old_by_key.emplace(key_for(old_children[i]), i);
    // The old index each new child keeps, or `none` when it is inserted or
    // recreated because its materialization shape changed.
    std::vector<std::size_t> source(new_children.size(), none);
    std::vector<bool> survives(old_children.size(), false);

    // First classify the old materializations.  All removals must precede
    // moves/inserts: otherwise an index-based consumer can move a child and
    // then remove that same slot, deleting the wrong identity.
    for (std::size_t new_index = 0; new_index < new_children.size(); ++new_index) {
        const auto it = old_by_key.find(key_for(new_children[new_index]));
        if (it == old_by_key.end() ||
            !shape_compatible(old_children[it->second], new_children[new_index]))
            continue;
        source[new_index] = it->second;
        survives[it->second] = true;
    }

    for (std::size_t old_index = old_children.size(); old_index-- > 0;) {
        if (!survives[old_index])
            append_update(plan, DesignUpdateKind::removed, key_for(old_children[old_index]),
                          old_index, 0);
    }

    // Position of each survivor in the child vector once removals apply.
    std::vector<std::size_t> survivor_position(old_children.size(), none);
    std::size_t survivors = 0;
    for (std::size_t old_index = 0; old_index < old_children.size(); ++old_index) {
        if (survives[old_index])
            survivor_position[old_index] = survivors++;
    }

    // Survivors that keep their relative order stay where they are. A shared
    // prefix and suffix are kept unconditionally; the middle keeps a longest
    // increasing run of survivor positions.
    std::vector<bool> stays(new_children.size(), false);
    std::size_t prefix = 0;
    while (prefix < new_children.size() && source[prefix] == prefix) {
        stays[prefix] = true;
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < new_children.size() - prefix && suffix < old_children.size() - prefix) {
        const auto new_index = new_children.size() - 1 - suffix;
        if (source[new_index] != old_children.size() - 1 - suffix)
            break;
        stays[new_index] = true;
        ++suffix;
    }
    std::vector<std::size_t> middle_indices;
    std::vector<std::size_t> middle_positions;
    for (auto new_index = prefix; new_index < new_children.size() - suffix; ++new_index) {
        if (source[new_index] == none)
            continue;
        middle_indices.push_back(new_index);
        middle_positions.push_back(survivor_position[source[new_index]]);
    }
    for (const auto at : longest_increasing_subsequence(middle_positions))
        stays[middle_indices[at]] = true;

    // Every child that is placed (moved or inserted) goes directly after its
    // new-order predecessor, so it lands after the nearest staying child
    // before it, in new order. Give each staying survivor a slot followed by
    // slots for the children placed after it; a leading group precedes all
    // survivors. Slot order is then child-vector order at every step.
    std::vector<std::size_t> anchor(new_children.size(), 0);
    std::vector<std::size_t> rank(new_children.size(), 0);
    std::vector<std::size_t> placed_after(survivors + 1, 0);
    std::size_t current_anchor = 0;
    for (std::size_t new_index = 0; new_index < new_children.size(); ++new_index) {
        if (stays[new_index]) {
            current_anchor = survivor_position[source[new_index]] + 1;
        } else {
            anchor[new_index] = current_anchor;
            rank[new_index] = ++placed_after[current_anchor];
        }
    }
    // first_slot[a] is the first slot of group a; group a > 0 opens with the
    // slot of survivor a - 1 at its old position.
    std::vector<std::size_t> first_slot(survivors + 2, 0);
    for (std::size_t group = 0; group <= survivors; ++group)
        first_slot[group + 1] = first_slot[group] + (group > 0 ? 1 : 0) + placed_after[group];
    const auto survivor_slot = [&](std::size_t position) { return first_slot[position + 1]; };
    const auto placed_slot = [&](std::size_t new_index) {
        const auto group = anchor[new_index];
        return first_slot[group] + (group > 0 ? 1 : 0) + rank[new_index] - 1;
    };

    SlotPositions positions(first_slot[survivors + 1]);
    for (std::size_t position = 0; position < survivors; ++position)
        positions.add(survivor_slot(position), 1);

    for (std::size_t new_index = 0; new_index < new_children.size(); ++new_index) {
        const auto& key = key_for(new_children[new_index]);
        const auto old_index = source[new_index];
        if (stays[new_index]) {
            append_update(plan, DesignUpdateKind::retained, key, old_index, new_index);
            continue;
        }
        const auto slot = placed_slot(new_index);
        if (old_index == none) {
            append_update(plan, DesignUpdateKind::inserted, key, 0, positions.before(slot));
        } else {
            const auto from_slot = survivor_slot(survivor_position[old_index]);
            const auto from = positions.before(from_slot);
            positions.add(from_slot, -1);
            append_update(plan, DesignUpdateKind::moved, key, from, positions.before(slot));
        }
        positions.add(slot, 1);
    }
    return plan;
}

std::string_view to_string(DesignKeyDiagnosticReason reason) noexcept {
    switch (reason) {
    case DesignKeyDiagnosticReason::missing:
        return "missing";
    case DesignKeyDiagnosticReason::duplicate:
        return "duplicate";
    case DesignKeyDiagnosticReason::mixed:
        return "mixed";
    }
    return "missing";
}

std::vector<DesignSlot> classify_design_slots(const IRNode& node) {
    std::vector<DesignSlot> slots;
    append_node_slots(node, slots);
    if (slots.empty())
        slots.push_back({DesignSlotKind::static_visual, {}, {}});
    std::sort(slots.begin(), slots.end(), [](const DesignSlot& lhs, const DesignSlot& rhs) {
        if (lhs.route_id != rhs.route_id)
            return lhs.route_id < rhs.route_id;
        if (slot_kind_rank(lhs.kind) != slot_kind_rank(rhs.kind))
            return slot_kind_rank(lhs.kind) < slot_kind_rank(rhs.kind);
        return lhs.key < rhs.key;
    });
    return slots;
}
} // namespace pulp::view
