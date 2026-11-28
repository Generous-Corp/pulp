#pragma once

// Windows UIA mapping for Pulp AccessRole.
//
// Pure-constant header — no UIAutomationCore.h dependency — so the same
// mapping compiles on every platform and can be unit-tested offline.
// The Windows accessibility provider (accessibility_win.cpp) consumes
// these constants to populate IRawElementProviderSimple properties.
//
// UIA control type and pattern IDs are stable across Windows versions
// (documented in UIAutomationCore.h). They're written here as named
// constants so every call site becomes grep-able.

#include <pulp/view/view.hpp>

#include <array>
#include <cstdint>

namespace pulp::view::uia {

// ── UIA Control Type IDs (subset Pulp exposes) ───────────────────────
// Full list in <UIAutomationCore.h>. We map the roles that Pulp declares
// today; additional roles (tree, table, text, menu) land as widgets need
// them.
inline constexpr int32_t kControlTypeButton      = 50000;
inline constexpr int32_t kControlTypeCheckBox    = 50002;
inline constexpr int32_t kControlTypeComboBox    = 50003;
inline constexpr int32_t kControlTypeEdit        = 50004;
inline constexpr int32_t kControlTypeHyperlink   = 50005;
inline constexpr int32_t kControlTypeImage       = 50006;
inline constexpr int32_t kControlTypeListItem    = 50007;
inline constexpr int32_t kControlTypeList        = 50008;
inline constexpr int32_t kControlTypeMenu        = 50009;
inline constexpr int32_t kControlTypeMenuItem    = 50011;
inline constexpr int32_t kControlTypeProgressBar = 50012;
inline constexpr int32_t kControlTypeRadioButton = 50013;
inline constexpr int32_t kControlTypeScrollBar   = 50014;
inline constexpr int32_t kControlTypeSlider      = 50015;
inline constexpr int32_t kControlTypeTab         = 50018;
inline constexpr int32_t kControlTypeTabItem     = 50019;
inline constexpr int32_t kControlTypeText        = 50020;
// UIA_CustomControlTypeId is 50025. It was written as 50033 here, which is
// UIA_PaneControlTypeId — every unroled fragment claimed to be a Pane.
inline constexpr int32_t kControlTypeCustom      = 50025;
inline constexpr int32_t kControlTypeGroup       = 50026;
inline constexpr int32_t kControlTypeDataItem    = 50029;
inline constexpr int32_t kControlTypeWindow      = 50032;
inline constexpr int32_t kControlTypeTable       = 50036;

// ── UIA Pattern IDs the provider may advertise per role ──────────────
//
// ONLY the patterns PulpFragmentProvider actually implements are listed here.
// The provider's GetPatternProvider (platform/win/accessibility_win.cpp)
// returns exactly two interfaces — IRangeValueProvider and IValueProvider — so
// those are the only two IDs that exist. Invoke / Toggle / Text were advertised
// here with no IInvokeProvider, IToggleProvider or ITextProvider anywhere in
// the codebase: Narrator would query the pattern, get a null interface, and
// the control would go dead. An advertised pattern is a promise; do not list
// one here until the provider can serve it.
inline constexpr int32_t kPatternValue       = 10002;
inline constexpr int32_t kPatternRangeValue  = 10003;

/// Map a Pulp AccessRole to the best-fit UIA control type. Unknown /
/// none → CustomControlType (the UIA spec's escape hatch).
constexpr int32_t role_to_control_type(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider:       return kControlTypeSlider;
        // A UIA CheckBox is the control type that carries the Toggle pattern.
        // `toggle` used to map to Button, which advertised Toggle on a control
        // type that has no toggle state — Narrator announced "button" and then
        // read a checked state the type does not define.
        case View::AccessRole::toggle:       return kControlTypeCheckBox;
        case View::AccessRole::label:        return kControlTypeText;
        case View::AccessRole::group:        return kControlTypeGroup;
        case View::AccessRole::meter:        return kControlTypeProgressBar;
        case View::AccessRole::image:        return kControlTypeImage;
        case View::AccessRole::button:       return kControlTypeButton;
        case View::AccessRole::link:         return kControlTypeHyperlink;
        case View::AccessRole::checkbox:     return kControlTypeCheckBox;
        case View::AccessRole::radio:        return kControlTypeRadioButton;
        case View::AccessRole::text_field:   return kControlTypeEdit;
        case View::AccessRole::combo_box:    return kControlTypeComboBox;
        case View::AccessRole::list:         return kControlTypeList;
        case View::AccessRole::list_item:    return kControlTypeListItem;
        case View::AccessRole::table:        return kControlTypeTable;
        // UIA has no Row control type; a table row is a DataItem, and cells are
        // ListItems under it (the DataGrid/Table pattern convention).
        case View::AccessRole::row:          return kControlTypeDataItem;
        case View::AccessRole::cell:         return kControlTypeListItem;
        case View::AccessRole::tab:          return kControlTypeTabItem;
        case View::AccessRole::tab_list:     return kControlTypeTab;
        case View::AccessRole::menu:         return kControlTypeMenu;
        case View::AccessRole::menu_item:    return kControlTypeMenuItem;
        case View::AccessRole::progress_bar: return kControlTypeProgressBar;
        case View::AccessRole::dialog:       return kControlTypeWindow;
        // UIA has no Heading control type; Text + a HeadingLevel property is
        // the convention. HeadingLevel is not populated yet.
        case View::AccessRole::heading:      return kControlTypeText;
        case View::AccessRole::scroll_bar:   return kControlTypeScrollBar;
        case View::AccessRole::none:         return kControlTypeCustom;
    }
    return kControlTypeCustom;
}

/// Pattern IDs the provider should advertise for a given role. Returns
/// up to N valid pattern IDs + the count. Small-array return keeps the
/// hot path allocation-free.
///
/// This is the role's CEILING, not the answer: a role only gets a pattern if
/// the concrete View behind it also has a source for it. See
/// exposes_range_value() / exposes_value() — the provider gates on those.
struct PatternSet {
    std::array<int32_t, 4> ids{};
    int count = 0;
};

constexpr PatternSet patterns_for_role(View::AccessRole role) {
    PatternSet s{};
    switch (role) {
        case View::AccessRole::slider:
        case View::AccessRole::scroll_bar:
            // Both: RangeValue reports min/max/value; Value reports the
            // widget's display string ("-6.0 dB").
            s.ids[s.count++] = kPatternRangeValue;
            s.ids[s.count++] = kPatternValue;
            break;
        case View::AccessRole::meter:
        case View::AccessRole::progress_bar:
            // Read-only gauge: RangeValue only (get_IsReadOnly = TRUE).
            s.ids[s.count++] = kPatternRangeValue;
            break;
        case View::AccessRole::text_field:
        case View::AccessRole::combo_box:
            // Value carries the text content / selected item. ITextProvider
            // (caret, ranges, attributes) is NOT implemented, so the Text
            // pattern is not advertised.
            s.ids[s.count++] = kPatternValue;
            break;
        // Every remaining role's defining pattern (Invoke for button/link/
        // menu_item, Toggle for toggle/checkbox, SelectionItem for radio/tab/
        // list_item, ExpandCollapse for the combo box's dropdown, Grid/Table
        // for table) is NOT implemented by PulpFragmentProvider. They advertise
        // NO pattern — the role name is correct, the interactions are the
        // follow-up. Roles are named exhaustively (no default:) so a new role
        // cannot silently inherit "no patterns".
        case View::AccessRole::toggle:
        case View::AccessRole::checkbox:
        case View::AccessRole::button:
        case View::AccessRole::link:
        case View::AccessRole::menu_item:
        case View::AccessRole::radio:
        case View::AccessRole::tab:
        case View::AccessRole::label:
        case View::AccessRole::heading:
        case View::AccessRole::list:
        case View::AccessRole::list_item:
        case View::AccessRole::table:
        case View::AccessRole::row:
        case View::AccessRole::cell:
        case View::AccessRole::tab_list:
        case View::AccessRole::menu:
        case View::AccessRole::dialog:
        case View::AccessRole::group:
        case View::AccessRole::image:
        case View::AccessRole::none:
            break;
    }
    return s;
}

/// True if a role's pattern set advertises the RangeValue pattern, i.e.
/// the per-widget fragment MAY expose IRangeValueProvider. Pure helper so the
/// COM provider and the offline test agree on which roles are range roles.
constexpr bool role_supports_range_value(View::AccessRole role) {
    const PatternSet pats = patterns_for_role(role);
    for (int i = 0; i < pats.count; ++i) {
        if (pats.ids[i] == kPatternRangeValue) return true;
    }
    return false;
}

/// True if a role advertises the Value pattern (IValueProvider). A
/// slider exposes both Value and RangeValue per Win UIA convention so a
/// reader can announce either a string ("-6 dB") or the normalized
/// fraction. Meter is read-only progress and exposes RangeValue only.
constexpr bool role_supports_value(View::AccessRole role) {
    const PatternSet pats = patterns_for_role(role);
    for (int i = 0; i < pats.count; ++i) {
        if (pats.ids[i] == kPatternValue) return true;
    }
    return false;
}

// ── Source-gated pattern availability ────────────────────────────────
//
// A role says what a pattern COULD be served; the View says whether it can
// actually serve it. Both must be true. Advertising RangeValue on a
// progress bar with no AccessibilityValueInterface made get_Value /
// get_Minimum / get_Maximum all return 0.0 — Narrator announced 0 at any
// progress, and min == max == 0 is a degenerate range that divides by zero in
// any client computing (v - min) / (max - min).
//
// Pure predicates so the offline test can assert them: accessibility_win.cpp
// (Windows-only TU, never compiled on the required macOS gate) does nothing but
// call these.

/// IRangeValueProvider needs numeric min/max/current — i.e. the View must
/// implement AccessibilityValueInterface.
constexpr bool exposes_range_value(View::AccessRole role,
                                   bool has_value_interface) {
    return role_supports_range_value(role) && has_value_interface;
}

/// IValueProvider::get_Value needs SOME string: a value interface's display
/// string, a text interface's content, or the app-set access_value slot.
constexpr bool exposes_value(View::AccessRole role,
                             bool has_value_interface,
                             bool has_text_interface,
                             bool has_value_string) {
    return role_supports_value(role) &&
           (has_value_interface || has_text_interface || has_value_string);
}

/// IsReadOnly must mean exactly "SetValue will fail". The provider can write
/// through an AccessibilityValueInterface (on a role that advertises Value —
/// a meter is read-only by role) or through an editable
/// AccessibilityTextInterface. Anything else is read-only. Reporting
/// VARIANT_FALSE ("editable") while SetValue returns UIA_E_NOTSUPPORTED told
/// Narrator the field was editable and then rejected every edit.
constexpr bool is_read_only(View::AccessRole role,
                            bool has_value_interface,
                            bool has_editable_text_interface) {
    const bool writable_value =
        has_value_interface && role_supports_value(role);
    return !(writable_value || has_editable_text_interface);
}

// ── Per-widget fragment runtime IDs ───────────────────────────────────
//
// Every UIA fragment must return a process-stable runtime ID so clients
// can compare element identity across calls. The first element of a
// runtime ID array is conventionally UiaAppendRuntimeId (3) for
// provider-supplied fragments; the remainder is a provider-chosen
// unique key. We derive the key from the fragment's depth-first index
// in the View tree, which is stable for the lifetime of the tree the
// provider was built against (the provider is rebuilt on structural
// change, which also raises StructureChanged so clients re-query).
//
// UiaAppendRuntimeId is documented as the integer constant 3; named
// here so the COM TU and the offline test share one definition without
// pulling in UIAutomationCore.h.
inline constexpr int32_t kUiaAppendRuntimeId = 3;

/// Build the two-element runtime-id key for the fragment at `index`
/// (its depth-first position among accessible fragments, root excluded).
/// Returns {UiaAppendRuntimeId, 1 + index}: the +1 keeps every key
/// strictly positive and distinct from a bare append-id sentinel.
struct RuntimeId {
    std::array<int32_t, 2> ids{};
    static constexpr int count = 2;
};

constexpr RuntimeId runtime_id_for_index(int index) {
    RuntimeId rid{};
    rid.ids[0] = kUiaAppendRuntimeId;
    rid.ids[1] = 1 + index;
    return rid;
}

/// Clamp a raw value into [lo, hi] then map to the [0, 1] fraction UIA's
/// Value pattern reports for a range control when no value-string is
/// available. Degenerate ranges (hi <= lo) report 0. Pure arithmetic so
/// the COM IValueProvider path stays trivial and is covered offline.
constexpr double normalized_value_fraction(double current,
                                           double lo,
                                           double hi) {
    if (!(hi > lo)) return 0.0;
    if (current <= lo) return 0.0;
    if (current >= hi) return 1.0;
    return (current - lo) / (hi - lo);
}

}  // namespace pulp::view::uia
