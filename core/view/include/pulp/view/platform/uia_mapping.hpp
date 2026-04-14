#pragma once

// Windows UIA mapping for Pulp AccessRole (workstream 04 slice 4.1a).
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

}  // namespace pulp::view::uia
