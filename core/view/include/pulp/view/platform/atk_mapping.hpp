#pragma once

// Linux ATK / AT-SPI mapping for Pulp AccessRole (workstream 04 slice 4.2a).
//
// Pure-constant header — no atk/atk.h dependency — so the mapping
// compiles and can be unit-tested on every platform. The Linux
// accessibility provider (accessibility_linux.cpp) consumes these
// constants when it registers with the AT-SPI bridge.
//
// Values come from AtkRole in atk/atk.h. They're stable across ATK
// versions and intentionally written as named constants here so call
// sites become grep-able and a future refactor can't silently drift.

#include <pulp/view/view.hpp>

#include <array>
#include <cstdint>

namespace pulp::view::atk {

// ── AtkRole IDs (subset Pulp exposes) ────────────────────────────────
//
// Matched against the AtkRole enum in atk/atk.h. Only the roles Pulp
// currently declares are listed; extending AccessRole adds rows here.
// Values verified against modern atk.h (ATK >= 2.30). Earlier drafts of
// this header hard-coded incorrect numbers — AT-SPI clients would have
// announced slider as KEYBOARD_KEY (34) and toggle as DATE_EDITOR (42).
// Fixed per the #204 review.
inline constexpr int32_t kRoleImage        = 21;
inline constexpr int32_t kRoleLabel        = 24;
inline constexpr int32_t kRolePushButton   = 29;
inline constexpr int32_t kRoleProgressBar  = 33;
inline constexpr int32_t kRolePanel        = 35;   // group container
inline constexpr int32_t kRoleSlider       = 50;   // was incorrectly 34
inline constexpr int32_t kRoleToggleButton = 61;   // was incorrectly 42
inline constexpr int32_t kRoleUnknown      = 66;   // was incorrectly 0

// ── AtkInterface flags the provider advertises per role ──────────────
//
// AT-SPI clients query an object for the interfaces it implements.
// A slider must expose AtkValue so Orca / Speakup can speak its
// min/max/current. The bits here map 1:1 to the set of GType-derived
// AtkInterface classes a provider implements.
inline constexpr uint32_t kInterfaceComponent = 1u << 0;
inline constexpr uint32_t kInterfaceValue     = 1u << 1;
inline constexpr uint32_t kInterfaceAction    = 1u << 2;
inline constexpr uint32_t kInterfaceText      = 1u << 3;
inline constexpr uint32_t kInterfaceImage     = 1u << 4;

constexpr int32_t role_to_atk(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return kRoleSlider;
        case View::AccessRole::toggle: return kRoleToggleButton;
        case View::AccessRole::label:  return kRoleLabel;
        case View::AccessRole::group:  return kRolePanel;
        case View::AccessRole::meter:  return kRoleProgressBar;
        case View::AccessRole::image:  return kRoleImage;
        case View::AccessRole::none:   return kRoleUnknown;
    }
    return kRoleUnknown;
}

/// Bitset of AtkInterface flags the provider should advertise for a
/// given role. Callers AND-test with the kInterface* constants.
constexpr uint32_t interfaces_for_role(View::AccessRole role) {
    // Every visible view exposes AtkComponent (size + screen position).
    uint32_t flags = kInterfaceComponent;
    switch (role) {
        case View::AccessRole::slider:
        case View::AccessRole::meter:
            flags |= kInterfaceValue;
            break;
        case View::AccessRole::toggle:
            flags |= kInterfaceAction;
            break;
        case View::AccessRole::label:
            flags |= kInterfaceText;
            break;
        case View::AccessRole::image:
            flags |= kInterfaceImage;
            break;
        case View::AccessRole::group:
        case View::AccessRole::none:
            // Component only.
            break;
    }
    return flags;
}

}  // namespace pulp::view::atk
