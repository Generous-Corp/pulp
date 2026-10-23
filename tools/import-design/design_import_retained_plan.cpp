#include "tools/import-design/design_import_retained_plan.hpp"

#include <pulp/view/data_display.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <unordered_map>

namespace pulp::view::retained {
namespace {

struct AnchorEntry {
    View* view = nullptr;
    std::size_t matches = 0;
};

void index_anchors(View& view, std::unordered_map<std::string, AnchorEntry>& index) {
    if (!view.anchor_id().empty()) {
        auto& entry = index[view.anchor_id()];
        if (entry.view == nullptr)
            entry.view = &view;
        ++entry.matches;
    }
    for (std::size_t i = 0; i < view.child_count(); ++i)
        index_anchors(*view.child_at(i), index);
}

bool scrolls(const IRNode& node) {
    const auto& overflow = node.style.overflow;
    return overflow && (*overflow == "scroll" || *overflow == "auto");
}

// Mirrors the materializer's normalization of audio_default so a typed write
// lands on the same widget value a rematerialization of the edited IR would.
float normalize(const Slot& slot, float value) {
    if (slot.audio_max > slot.audio_min)
        return std::clamp((value - slot.audio_min) / (slot.audio_max - slot.audio_min), 0.0f, 1.0f);
    return std::clamp(value, 0.0f, 1.0f);
}

class Compiler {
  public:
    Compiler(View& root, std::vector<Slot>& slots, std::vector<std::string>& diagnostics)
        : slots_(slots), diagnostics_(diagnostics) {
        index_anchors(root, index_);
    }

    void visit(const IRNode& node, bool parent_scrolls) {
        if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
            const auto& anchor = *node.stable_anchor_id;
            if (const auto kind = widget_slot_kind(node))
                add(node, anchor, *kind);
            if (parent_scrolls)
                add(node, anchor, SlotKind::offset);
        }
        const bool child_parent_scrolls = scrolls(node);
        for (const auto& child : node.children)
            visit(child, child_parent_scrolls);
    }

  private:
    static std::optional<SlotKind> widget_slot_kind(const IRNode& node) {
        switch (node.audio_widget) {
        case AudioWidgetType::knob:
        case AudioWidgetType::fader:
            return SlotKind::scalar;
        case AudioWidgetType::meter:
            return SlotKind::level;
        case AudioWidgetType::xy_pad:
            return SlotKind::vector;
        default:
            return std::nullopt;
        }
    }

    void refuse(const std::string& anchor, const char* reason) {
        diagnostics_.push_back(anchor + ": " + reason);
    }

    void add(const IRNode& node, const std::string& anchor, SlotKind kind) {
        const auto found = index_.find(anchor);
        if (found == index_.end() || found->second.view == nullptr) {
            refuse(anchor, "no materialized view carries this anchor");
            return;
        }
        if (found->second.matches != 1) {
            refuse(anchor, "anchor is not unique in the materialized tree");
            return;
        }
        Slot slot;
        slot.anchor = anchor;
        slot.kind = kind;
        slot.view = found->second.view;
        slot.audio_min = node.audio_min;
        slot.audio_max = node.audio_max;
        switch (kind) {
        case SlotKind::scalar:
            slot.knob = dynamic_cast<Knob*>(slot.view);
            slot.fader = dynamic_cast<Fader*>(slot.view);
            slot.range = dynamic_cast<RangeSlider*>(slot.view);
            if (!slot.knob && !slot.fader && !slot.range) {
                refuse(anchor, "scalar slot view is not a Knob, Fader, or RangeSlider");
                return;
            }
            break;
        case SlotKind::level:
            slot.level = dynamic_cast<LevelDisplay*>(slot.view);
            if (!slot.level) {
                refuse(anchor, "level slot view is not a LevelDisplay");
                return;
            }
            break;
        case SlotKind::vector:
            slot.pad = dynamic_cast<XYPad*>(slot.view);
            if (!slot.pad) {
                refuse(anchor, "vector slot view is not an XYPad");
                return;
            }
            break;
        case SlotKind::offset:
            break;
        }
        slots_.push_back(std::move(slot));
    }

    std::unordered_map<std::string, AnchorEntry> index_;
    std::vector<Slot>& slots_;
    std::vector<std::string>& diagnostics_;
};

} // namespace

RetainedPlan RetainedPlan::compile(const DesignIR& ir, View& root) {
    RetainedPlan plan;
    Compiler compiler(root, plan.slots_, plan.diagnostics_);
    compiler.visit(ir.root, false);
    return plan;
}

std::optional<std::uint32_t> RetainedPlan::find(std::string_view anchor, SlotKind kind) const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].kind == kind && slots_[i].anchor == anchor)
            return static_cast<std::uint32_t>(i);
    }
    return std::nullopt;
}

ApplyResult RetainedPlan::apply(std::span<const SlotUpdate> updates) const {
    ApplyResult result;
    for (const auto& update : updates) {
        if (update.slot >= slots_.size()) {
            ++result.rejected;
            continue;
        }
        const auto& slot = slots_[update.slot];
        switch (slot.kind) {
        case SlotKind::scalar: {
            const float value = normalize(slot, update.a);
            if (slot.knob)
                slot.knob->set_value(value);
            else if (slot.fader)
                slot.fader->set_value(value);
            else
                slot.range->set_value(value);
            break;
        }
        case SlotKind::level:
            slot.level->set_levels(normalize(slot, update.a), update.b);
            break;
        case SlotKind::vector:
            slot.pad->set_x(update.a);
            slot.pad->set_y(update.b);
            break;
        case SlotKind::offset:
            slot.view->set_left(update.a);
            slot.view->set_top(update.b);
            result.needs_layout = true;
            break;
        }
        ++result.applied;
    }
    return result;
}

} // namespace pulp::view::retained
