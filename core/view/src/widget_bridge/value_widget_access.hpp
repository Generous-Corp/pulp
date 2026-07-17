#pragma once

// Canonical scalar-value access for the bridge's value widgets.
//
// Snapshot, restore, and any other caller that needs "the one float this widget
// carries" routes through these two helpers instead of re-rolling the
// dynamic_cast ladder. Keeping a single ladder is what makes adding a value
// widget a one-line change rather than a hunt for every divergent copy.
//
// The ladder order is part of the contract: the first matching type wins, and
// get/set stay symmetric — every type readable by try_get_scalar_value is
// writable by try_set_scalar_value and vice versa. Widgets whose value is not a
// scalar (XYPad) or whose scalar is an index with silent-set semantics
// (ComboBox, SegmentedControl) are deliberately NOT here: their call sites
// handle them explicitly after this helper declines.

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

namespace pulp::view {

/// Read `view`'s scalar value into `out`. Boolean widgets report 1.0f / 0.0f.
/// Returns false — leaving `out` untouched — for a null view or one that carries
/// no scalar value.
inline bool try_get_scalar_value(View* view, float& out) {
    if (auto* k = dynamic_cast<Knob*>(view)) { out = k->value(); return true; }
    else if (auto* f = dynamic_cast<Fader*>(view)) { out = f->value(); return true; }
    else if (auto* r = dynamic_cast<RangeSlider*>(view)) { out = r->value(); return true; }
    else if (auto* t = dynamic_cast<Toggle*>(view)) { out = t->is_on() ? 1.0f : 0.0f; return true; }
    else if (auto* cb = dynamic_cast<Checkbox*>(view)) { out = cb->is_checked() ? 1.0f : 0.0f; return true; }
    else if (auto* tb = dynamic_cast<ToggleButton*>(view)) { out = tb->is_on() ? 1.0f : 0.0f; return true; }
    return false;
}

/// Write `value` onto `view`'s scalar surface. Boolean widgets read > 0.5 as on.
/// Returns false for a null view or one that carries no scalar value.
inline bool try_set_scalar_value(View* view, float value) {
    if (auto* k = dynamic_cast<Knob*>(view)) { k->set_value(value); return true; }
    else if (auto* f = dynamic_cast<Fader*>(view)) { f->set_value(value); return true; }
    else if (auto* r = dynamic_cast<RangeSlider*>(view)) { r->set_value(value); return true; }
    else if (auto* t = dynamic_cast<Toggle*>(view)) { t->set_on(value > 0.5f); return true; }
    else if (auto* cb = dynamic_cast<Checkbox*>(view)) { cb->set_checked(value > 0.5f); return true; }
    else if (auto* tb = dynamic_cast<ToggleButton*>(view)) { tb->set_on(value > 0.5f); return true; }
    return false;
}

} // namespace pulp::view
