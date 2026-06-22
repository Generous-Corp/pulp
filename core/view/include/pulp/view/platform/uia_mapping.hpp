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
inline constexpr int32_t kControlTypeButton   = 50000;
inline constexpr int32_t kControlTypeImage    = 50006;
inline constexpr int32_t kControlTypeProgressBar = 50012;
inline constexpr int32_t kControlTypeSlider   = 50015;
inline constexpr int32_t kControlTypeText     = 50020;
inline constexpr int32_t kControlTypeGroup    = 50026;
inline constexpr int32_t kControlTypeCustom   = 50033;

// ── UIA Pattern IDs the provider may advertise per role ──────────────
// Patterns are the contract for screen-reader interactions — a slider
// MUST support RangeValue so the reader can announce min/max/value.
inline constexpr int32_t kPatternInvoke      = 10000;
inline constexpr int32_t kPatternValue       = 10002;
inline constexpr int32_t kPatternRangeValue  = 10003;
inline constexpr int32_t kPatternToggle      = 10015;
inline constexpr int32_t kPatternText        = 10014;

/// Map a Pulp AccessRole to the best-fit UIA control type. Unknown /
/// none → CustomControlType (the UIA spec's escape hatch).
constexpr int32_t role_to_control_type(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return kControlTypeSlider;
        case View::AccessRole::toggle: return kControlTypeButton;
        case View::AccessRole::label:  return kControlTypeText;
        case View::AccessRole::group:  return kControlTypeGroup;
        case View::AccessRole::meter:  return kControlTypeProgressBar;
        case View::AccessRole::image:  return kControlTypeImage;
        case View::AccessRole::none:   return kControlTypeCustom;
    }
    return kControlTypeCustom;
}

/// Pattern IDs the provider should advertise for a given role. Returns
/// up to N valid pattern IDs + the count. Small-array return keeps the
/// hot path allocation-free.
struct PatternSet {
    std::array<int32_t, 4> ids{};
    int count = 0;
};

constexpr PatternSet patterns_for_role(View::AccessRole role) {
    PatternSet s{};
    switch (role) {
        case View::AccessRole::slider:
            s.ids[s.count++] = kPatternRangeValue;
            s.ids[s.count++] = kPatternValue;
            break;
        case View::AccessRole::toggle:
            s.ids[s.count++] = kPatternToggle;
            s.ids[s.count++] = kPatternInvoke;
            break;
        case View::AccessRole::label:
            s.ids[s.count++] = kPatternText;
            break;
        case View::AccessRole::meter:
            s.ids[s.count++] = kPatternRangeValue;
            break;
        case View::AccessRole::group:
        case View::AccessRole::image:
        case View::AccessRole::none:
            // No patterns — provider advertises role only.
            break;
    }
    return s;
}

/// True if a role's pattern set advertises the RangeValue pattern, i.e.
/// the per-widget fragment should expose IRangeValueProvider (slider /
/// meter today). Pure helper so the COM provider and the offline test
/// agree on which fragments are range widgets.
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
