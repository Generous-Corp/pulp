#pragma once

// Linux AT-SPI2 mapping for Pulp AccessRole.
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

#include <pulp/view/accessibility.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>

namespace pulp::view::atspi {

// ── AtspiRole values (subset Pulp exposes) ───────────────────────────────────
// The full enumeration lives in the AT-SPI2 IDL. These are the AtspiRole
// numbers (NOT AtkRole) for the roles Pulp declares today. Additional roles
// land as widgets need them.
inline constexpr uint32_t kRoleCheckBox     = 7;    // ATSPI_ROLE_CHECK_BOX
inline constexpr uint32_t kRoleComboBox     = 11;   // ATSPI_ROLE_COMBO_BOX
inline constexpr uint32_t kRoleDialog       = 16;   // ATSPI_ROLE_DIALOG
inline constexpr uint32_t kRoleImage        = 27;   // ATSPI_ROLE_IMAGE
inline constexpr uint32_t kRoleLabel        = 29;   // ATSPI_ROLE_LABEL
inline constexpr uint32_t kRoleList         = 31;   // ATSPI_ROLE_LIST
inline constexpr uint32_t kRoleListItem     = 32;   // ATSPI_ROLE_LIST_ITEM
inline constexpr uint32_t kRoleMenu         = 33;   // ATSPI_ROLE_MENU
inline constexpr uint32_t kRoleMenuItem     = 35;   // ATSPI_ROLE_MENU_ITEM
inline constexpr uint32_t kRolePageTab      = 37;   // ATSPI_ROLE_PAGE_TAB
inline constexpr uint32_t kRolePageTabList  = 38;   // ATSPI_ROLE_PAGE_TAB_LIST
inline constexpr uint32_t kRolePanel        = 39;   // ATSPI_ROLE_PANEL
inline constexpr uint32_t kRoleProgressBar  = 42;   // ATSPI_ROLE_PROGRESS_BAR
inline constexpr uint32_t kRolePushButton   = 43;   // ATSPI_ROLE_PUSH_BUTTON
inline constexpr uint32_t kRoleRadioButton  = 44;   // ATSPI_ROLE_RADIO_BUTTON
inline constexpr uint32_t kRoleScrollBar    = 48;   // ATSPI_ROLE_SCROLL_BAR
inline constexpr uint32_t kRoleSlider       = 51;   // ATSPI_ROLE_SLIDER
inline constexpr uint32_t kRoleTable        = 55;   // ATSPI_ROLE_TABLE
inline constexpr uint32_t kRoleTableCell    = 56;   // ATSPI_ROLE_TABLE_CELL
inline constexpr uint32_t kRoleToggleButton = 62;   // ATSPI_ROLE_TOGGLE_BUTTON
inline constexpr uint32_t kRoleApplication  = 75;   // ATSPI_ROLE_APPLICATION
inline constexpr uint32_t kRoleEntry        = 79;   // ATSPI_ROLE_ENTRY
inline constexpr uint32_t kRoleHeading      = 83;   // ATSPI_ROLE_HEADING
inline constexpr uint32_t kRoleLink         = 88;   // ATSPI_ROLE_LINK
inline constexpr uint32_t kRoleTableRow     = 90;   // ATSPI_ROLE_TABLE_ROW
inline constexpr uint32_t kRoleInvalid      = 0;    // ATSPI_ROLE_INVALID

/// Map a Pulp AccessRole to the best-fit AtspiRole number. Unknown / none →
/// PANEL (a generic container — AT-SPI has no "custom" escape hatch the way
/// UIA does, and PANEL is the conventional neutral grouping role).
constexpr uint32_t role_to_atspi_role(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider:       return kRoleSlider;
        case View::AccessRole::toggle:       return kRoleToggleButton;
        case View::AccessRole::label:        return kRoleLabel;
        case View::AccessRole::group:        return kRolePanel;
        // AT-SPI also has LEVEL_BAR, which is arguably the better fit for a
        // read-only gauge, but its enum number sits past the range this table
        // has verified against the AT-SPI2 IDL, so `meter` keeps PROGRESS_BAR
        // (unchanged behavior) until it can be checked against atspi headers.
        case View::AccessRole::meter:        return kRoleProgressBar;
        case View::AccessRole::image:        return kRoleImage;
        case View::AccessRole::button:       return kRolePushButton;
        case View::AccessRole::link:         return kRoleLink;
        case View::AccessRole::checkbox:     return kRoleCheckBox;
        case View::AccessRole::radio:        return kRoleRadioButton;
        case View::AccessRole::text_field:   return kRoleEntry;
        case View::AccessRole::combo_box:    return kRoleComboBox;
        case View::AccessRole::list:         return kRoleList;
        case View::AccessRole::list_item:    return kRoleListItem;
        case View::AccessRole::table:        return kRoleTable;
        case View::AccessRole::row:          return kRoleTableRow;
        case View::AccessRole::cell:         return kRoleTableCell;
        case View::AccessRole::tab:          return kRolePageTab;
        case View::AccessRole::tab_list:     return kRolePageTabList;
        case View::AccessRole::menu:         return kRoleMenu;
        case View::AccessRole::menu_item:    return kRoleMenuItem;
        case View::AccessRole::progress_bar: return kRoleProgressBar;
        case View::AccessRole::dialog:       return kRoleDialog;
        case View::AccessRole::heading:      return kRoleHeading;
        case View::AccessRole::scroll_bar:   return kRoleScrollBar;
        case View::AccessRole::none:         return kRolePanel;
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
        case View::AccessRole::slider:       return "slider";
        case View::AccessRole::toggle:       return "toggle button";
        case View::AccessRole::label:        return "label";
        case View::AccessRole::group:        return "panel";
        case View::AccessRole::meter:        return "progress bar";
        case View::AccessRole::image:        return "image";
        case View::AccessRole::button:       return "push button";
        case View::AccessRole::link:         return "link";
        case View::AccessRole::checkbox:     return "check box";
        case View::AccessRole::radio:        return "radio button";
        case View::AccessRole::text_field:   return "entry";
        case View::AccessRole::combo_box:    return "combo box";
        case View::AccessRole::list:         return "list";
        case View::AccessRole::list_item:    return "list item";
        case View::AccessRole::table:        return "table";
        case View::AccessRole::row:          return "table row";
        case View::AccessRole::cell:         return "table cell";
        case View::AccessRole::tab:          return "page tab";
        case View::AccessRole::tab_list:     return "page tab list";
        case View::AccessRole::menu:         return "menu";
        case View::AccessRole::menu_item:    return "menu item";
        case View::AccessRole::progress_bar: return "progress bar";
        case View::AccessRole::dialog:       return "dialog";
        case View::AccessRole::heading:      return "heading";
        case View::AccessRole::scroll_bar:   return "scroll bar";
        case View::AccessRole::none:         return "panel";
    }
    return "panel";
}

// ── AtspiStateType values (subset) ───────────────────────────────────────────
// AT-SPI's GetState returns a 64-bit state bitfield marshalled as two uint32
// words (au of length 2: low word = states 0..31, high word = states 32..63).
// These are the AtspiStateType bit INDICES Pulp sets today. Helpers below build
// the two-word representation so the provider and the offline test agree.
//
// The numbers are the ORDINALS of AtspiStateType in at-spi2-core's
// atspi-constants.h (ATSPI_STATE_INVALID = 0, ATSPI_STATE_ACTIVE = 1, ...,
// ATSPI_STATE_LAST_DEFINED = 44) — the same enumeration Orca reads back off the
// bus. kStateShowing / kStateVisible used to carry 26 / 28, which are
// ATSPI_STATE_SINGLE_LINE and ATSPI_STATE_TRANSIENT: every Pulp accessible
// reported itself as neither showing nor visible (and as a single-line,
// transient object), which is exactly the state set Orca skips over. Locked by
// the ordinal table in test/test_atspi_mapping.cpp.
inline constexpr uint32_t kStateChecked       = 4;    // ATSPI_STATE_CHECKED
inline constexpr uint32_t kStateEditable      = 7;    // ATSPI_STATE_EDITABLE
inline constexpr uint32_t kStateEnabled       = 8;    // ATSPI_STATE_ENABLED
inline constexpr uint32_t kStateFocusable     = 11;   // ATSPI_STATE_FOCUSABLE
inline constexpr uint32_t kStateFocused       = 12;   // ATSPI_STATE_FOCUSED
inline constexpr uint32_t kStatePressed       = 20;   // ATSPI_STATE_PRESSED
inline constexpr uint32_t kStateSensitive     = 24;   // ATSPI_STATE_SENSITIVE
inline constexpr uint32_t kStateShowing       = 25;   // ATSPI_STATE_SHOWING
inline constexpr uint32_t kStateVisible       = 30;   // ATSPI_STATE_VISIBLE
inline constexpr uint32_t kStateIndeterminate = 32;   // ATSPI_STATE_INDETERMINATE
inline constexpr uint32_t kStateCheckable     = 41;   // ATSPI_STATE_CHECKABLE

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

/// Fold a View's check/press state into an AT-SPI state set. AT-SPI has no
/// "value" for a checkbox — the STATE bits are how Orca announces "checked" /
/// "not checked" / "partially checked", so a checkbox that only carries a role
/// reads as a plain toggle-button with nothing to say.
///
/// aria-checked → CHECKABLE + CHECKED; aria-pressed → PRESSED; "mixed" → the
/// INDETERMINATE bit with CHECKED/PRESSED left clear (ATK/AT-SPI's tri-state).
///
/// `from_checked` distinguishes the two ARIA slots (checked vs pressed); pass
/// what accessibility_toggle_state() resolved. Pure so the offline test asserts
/// the exact bits the Linux TU (never compiled on the macOS gate) will emit.
constexpr StateSet with_toggle_state(StateSet s, AccessToggleState state,
                                     bool from_checked) {
    if (state == AccessToggleState::unset) return s;
    if (from_checked) set_state(s, kStateCheckable);
    if (state == AccessToggleState::mixed) {
        set_state(s, kStateIndeterminate);
        return s;
    }
    if (state == AccessToggleState::on)
        set_state(s, from_checked ? kStateChecked : kStatePressed);
    return s;
}

/// EDITABLE — an editable text interface. Orca uses it to decide whether to
/// enter text-entry mode.
constexpr StateSet with_editable(StateSet s, bool editable) {
    if (editable) set_state(s, kStateEditable);
    return s;
}

}  // namespace pulp::view::atspi
