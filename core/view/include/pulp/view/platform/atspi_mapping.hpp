#pragma once

// Linux AT-SPI2 mapping for Pulp AccessRole (workstream 04 slice L7a-2).
//
// Pure-constant header — no AT-SPI / ATK runtime dependency — so the same
// mapping compiles on every platform and can be unit-tested offline. The Linux
// accessibility provider (accessibility_linux.cpp) consumes these constants
// when it answers org.a11y.atspi.Accessible.GetRole over D-Bus.
//
// AT-SPI exports roles as plain `uint32` enum NUMBERS on the wire (the
// AtspiRole / Atspi_Role enumeration in the published AT-SPI2 IDL). They are
// part of the wire protocol — a screen reader (Orca) reads the number and looks
// up its own label — so they are stable and safe to hard-code without pulling
// in libatspi headers. They DIFFER from the legacy ATK AtkRole numbers the
// pre-L7 stub hard-coded (AtkRole and AtspiRole are distinct enumerations);
// the table test locks the AT-SPI numbers so a future edit can't silently
// regress to the ATK values.
//
// Reference: AT-SPI2 `Accessible.GetRole` returns `u`; the AtspiRole values
// below are from the freedesktop AT-SPI2 enumeration (xml/Accessible role list).

#include <pulp/view/view.hpp>

#include <cstdint>

namespace pulp::view::atspi {

// ── AtspiRole values (subset Pulp exposes) ───────────────────────────────────
// The full enumeration lives in the AT-SPI2 IDL. These are the AtspiRole
// numbers (NOT AtkRole) for the roles Pulp declares today. Additional roles
// land as widgets need them.
inline constexpr uint32_t kRoleImage       = 27;   // ATSPI_ROLE_IMAGE
inline constexpr uint32_t kRoleLabel       = 29;   // ATSPI_ROLE_LABEL
inline constexpr uint32_t kRolePanel       = 54;   // ATSPI_ROLE_PANEL
inline constexpr uint32_t kRoleProgressBar = 51;   // ATSPI_ROLE_PROGRESS_BAR
inline constexpr uint32_t kRoleSlider      = 71;   // ATSPI_ROLE_SLIDER
inline constexpr uint32_t kRoleToggleButton = 79;  // ATSPI_ROLE_TOGGLE_BUTTON
inline constexpr uint32_t kRoleApplication = 75;   // ATSPI_ROLE_APPLICATION
inline constexpr uint32_t kRoleInvalid     = 0;    // ATSPI_ROLE_INVALID

/// Map a Pulp AccessRole to the best-fit AtspiRole number. Unknown / none →
/// PANEL (a generic container — AT-SPI has no "custom" escape hatch the way
/// UIA does, and PANEL is the conventional neutral grouping role).
constexpr uint32_t role_to_atspi_role(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return kRoleSlider;
        case View::AccessRole::toggle: return kRoleToggleButton;
        case View::AccessRole::label:  return kRoleLabel;
        case View::AccessRole::group:  return kRolePanel;
        case View::AccessRole::meter:  return kRoleProgressBar;
        case View::AccessRole::image:  return kRoleImage;
        case View::AccessRole::none:   return kRolePanel;
    }
    return kRolePanel;
}

/// The AT-SPI role NAME for a Pulp AccessRole, returned by
/// org.a11y.atspi.Accessible.GetRoleName (s). These are the canonical
/// lowercase-with-spaces role strings from the AT-SPI2 IDL (the same names
/// `atspi_role_get_name()` produces) so a screen reader that reads the name
/// rather than the numeric role still gets the conventional label. Kept in
/// lockstep with role_to_atspi_role() and locked by the offline table test.
constexpr const char* role_to_atspi_role_name(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return "slider";
        case View::AccessRole::toggle: return "toggle button";
        case View::AccessRole::label:  return "label";
        case View::AccessRole::group:  return "panel";
        case View::AccessRole::meter:  return "progress bar";
        case View::AccessRole::image:  return "image";
        case View::AccessRole::none:   return "panel";
    }
    return "panel";
}

// ── AtspiStateType values (subset) ───────────────────────────────────────────
// AT-SPI's GetState returns a 64-bit state bitfield marshalled as two uint32
// words (au of length 2: low word = states 0..31, high word = states 32..63).
// These are the AtspiStateType bit INDICES Pulp sets today. Helpers below build
// the two-word representation so the provider and the offline test agree.
inline constexpr uint32_t kStateEnabled   = 8;    // ATSPI_STATE_ENABLED
inline constexpr uint32_t kStateSensitive = 24;   // ATSPI_STATE_SENSITIVE
inline constexpr uint32_t kStateShowing   = 26;   // ATSPI_STATE_SHOWING
inline constexpr uint32_t kStateVisible   = 28;   // ATSPI_STATE_VISIBLE

/// Two-word AT-SPI state bitfield (the `au` GetState returns: [low, high]).
struct StateSet {
    uint32_t low = 0;
    uint32_t high = 0;
};

/// Set the bit for AtspiStateType `index` (0..63) in a two-word StateSet.
constexpr void set_state(StateSet& s, uint32_t index) {
    if (index < 32) {
        s.low |= (1u << index);
    } else {
        s.high |= (1u << (index - 32));
    }
}

/// True if AtspiStateType `index` is set in `s`.
constexpr bool has_state(const StateSet& s, uint32_t index) {
    if (index < 32) return (s.low & (1u << index)) != 0;
    return (s.high & (1u << (index - 32))) != 0;
}

/// The default state for an enabled, visible, showing accessible object — the
/// baseline every Pulp accessible (including the application root) reports.
constexpr StateSet default_states() {
    StateSet s{};
    set_state(s, kStateEnabled);
    set_state(s, kStateSensitive);
    set_state(s, kStateShowing);
    set_state(s, kStateVisible);
    return s;
}

}  // namespace pulp::view::atspi
