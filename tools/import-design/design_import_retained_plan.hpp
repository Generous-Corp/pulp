#pragma once

// Experimental retained typed update plan for a natively materialized DesignIR.
//
// This header is deliberately not part of the public pulp/view surface and its
// source is not compiled into any library: only the design-import benchmark
// builds it, as its `retained-plan` lane. The production baked-native path
// remains build_native_view_tree() plus direct widget setters.
//
// compile() runs once after materialization. It resolves every dynamic slot of
// the imported tree to its native view and typed setter, so a later state
// change is an indexed list of typed writes: no tree walk, no type dispatch by
// name, and no document re-apply. Structural edits are out of scope; a caller
// that changes the document rematerializes and compiles a new plan.

#include <pulp/view/design_ir.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {
class View;
class Knob;
class Fader;
class RangeSlider;
class XYPad;
class LevelDisplay;
} // namespace pulp::view

namespace pulp::view::retained {

enum class SlotKind : std::uint8_t {
    scalar, ///< Knob / Fader / RangeSlider value, in the node's audio_min..audio_max units.
    level,  ///< LevelDisplay primary (audio units) and secondary (peak, normalized).
    vector, ///< XYPad x and y, normalized.
    offset, ///< Left/top of a child of an overflow scroll/auto container, in px.
};

struct Slot {
    std::string anchor;
    SlotKind kind = SlotKind::scalar;
    View* view = nullptr;
    Knob* knob = nullptr;
    Fader* fader = nullptr;
    RangeSlider* range = nullptr;
    LevelDisplay* level = nullptr;
    XYPad* pad = nullptr;
    float audio_min = 0.0f;
    float audio_max = 1.0f;
};

/// One typed write. `a`/`b` meaning follows the slot kind: scalar uses `a`;
/// level uses primary=`a`, peak=`b`; vector uses x=`a`, y=`b`; offset uses
/// left=`a`, top=`b`.
struct SlotUpdate {
    std::uint32_t slot = 0;
    float a = 0.0f;
    float b = 0.0f;
};

struct ApplyResult {
    std::size_t applied = 0;
    std::size_t rejected = 0;
    /// True when an update moved geometry, so the caller must lay out again.
    bool needs_layout = false;
};

class RetainedPlan {
  public:
    /// Resolve the dynamic slots of `ir` against the view tree that
    /// build_native_view_tree() produced for it. Anchors that are missing or
    /// duplicated in the view tree, or whose view is not the expected widget
    /// type, are refused and named in diagnostics() rather than guessed.
    static RetainedPlan compile(const DesignIR& ir, View& root);

    std::optional<std::uint32_t> find(std::string_view anchor, SlotKind kind) const;
    ApplyResult apply(std::span<const SlotUpdate> updates) const;

    const std::vector<Slot>& slots() const noexcept {
        return slots_;
    }
    const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

  private:
    std::vector<Slot> slots_;
    std::vector<std::string> diagnostics_;
};

} // namespace pulp::view::retained
