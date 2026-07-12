// Tests for the Pulp → Linux AT-SPI2 mapping table.
// Pure-constant checks; the actual AT-SPI provider lives in
// core/view/platform/linux/accessibility_linux.cpp and is validated per platform
// (real AT stack) by VM integration tests.
//
// These lock the AtspiRole NUMBERS so a future edit can't silently regress
// to the legacy ATK AtkRole values the pre-L7 stub hard-coded (AtkRole and
// AtspiRole are distinct enumerations with different numeric values).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/platform/atspi_mapping.hpp>

#include <string_view>

using namespace pulp::view;
using namespace pulp::view::atspi;

TEST_CASE("role_to_atspi_role returns stable AtspiRole numbers", "[a11y][atspi]") {
    REQUIRE(role_to_atspi_role(View::AccessRole::slider) == kRoleSlider);
    REQUIRE(role_to_atspi_role(View::AccessRole::toggle) == kRoleToggleButton);
    REQUIRE(role_to_atspi_role(View::AccessRole::label)  == kRoleLabel);
    REQUIRE(role_to_atspi_role(View::AccessRole::group)  == kRolePanel);
    REQUIRE(role_to_atspi_role(View::AccessRole::meter)  == kRoleProgressBar);
    REQUIRE(role_to_atspi_role(View::AccessRole::image)  == kRoleImage);
    REQUIRE(role_to_atspi_role(View::AccessRole::none)   == kRolePanel);
}

TEST_CASE("AtspiRole numbers match the published AT-SPI2 enumeration",
          "[a11y][atspi]") {
    // These are AtspiRole values (the wire numbers Accessible.GetRole returns),
    // NOT the legacy AtkRole values. They are part of the AT-SPI2 protocol.
    REQUIRE(kRoleImage        == 27);
    REQUIRE(kRoleLabel        == 29);
    REQUIRE(kRolePanel        == 39);
    REQUIRE(kRoleProgressBar  == 42);
    REQUIRE(kRoleSlider       == 51);
    REQUIRE(kRoleToggleButton == 62);
    REQUIRE(kRoleApplication  == 75);

    // Guard against a regression back to the old ATK numbers (slider was 34,
    // label 24, panel 35, progress 33, image 21 under AtkRole) or the
    // post-application AT-SPI roles Pulp accidentally used in L7b.
    REQUIRE(kRoleSlider != 34u);
    REQUIRE(kRoleLabel != 24u);
    REQUIRE(kRoleSlider != 71u);
    REQUIRE(kRoleToggleButton != 79u);
}

TEST_CASE("expanded roles map to their own AtspiRole numbers", "[a11y][atspi]") {
    // Before the vocabulary expansion, TextButton was TOGGLE_BUTTON (62) and
    // ComboBox was SLIDER (51) — Orca announced "toggle button" and "slider".
    REQUIRE(role_to_atspi_role(View::AccessRole::button)       == kRolePushButton);
    REQUIRE(role_to_atspi_role(View::AccessRole::link)         == kRoleLink);
    REQUIRE(role_to_atspi_role(View::AccessRole::checkbox)     == kRoleCheckBox);
    REQUIRE(role_to_atspi_role(View::AccessRole::radio)        == kRoleRadioButton);
    REQUIRE(role_to_atspi_role(View::AccessRole::text_field)   == kRoleEntry);
    REQUIRE(role_to_atspi_role(View::AccessRole::combo_box)    == kRoleComboBox);
    REQUIRE(role_to_atspi_role(View::AccessRole::list)         == kRoleList);
    REQUIRE(role_to_atspi_role(View::AccessRole::list_item)    == kRoleListItem);
    REQUIRE(role_to_atspi_role(View::AccessRole::table)        == kRoleTable);
    REQUIRE(role_to_atspi_role(View::AccessRole::row)          == kRoleTableRow);
    REQUIRE(role_to_atspi_role(View::AccessRole::cell)         == kRoleTableCell);
    REQUIRE(role_to_atspi_role(View::AccessRole::tab)          == kRolePageTab);
    REQUIRE(role_to_atspi_role(View::AccessRole::tab_list)     == kRolePageTabList);
    REQUIRE(role_to_atspi_role(View::AccessRole::menu)         == kRoleMenu);
    REQUIRE(role_to_atspi_role(View::AccessRole::menu_item)    == kRoleMenuItem);
    REQUIRE(role_to_atspi_role(View::AccessRole::progress_bar) == kRoleProgressBar);
    REQUIRE(role_to_atspi_role(View::AccessRole::dialog)       == kRoleDialog);
    REQUIRE(role_to_atspi_role(View::AccessRole::heading)      == kRoleHeading);
    REQUIRE(role_to_atspi_role(View::AccessRole::scroll_bar)   == kRoleScrollBar);

    // No expanded role may silently fall back to PANEL (the `none` bucket).
    for (int i = 0; i <= static_cast<int>(View::AccessRole::scroll_bar); ++i) {
        const auto role = static_cast<View::AccessRole>(i);
        if (role == View::AccessRole::none || role == View::AccessRole::group) continue;
        INFO("role ordinal " << i);
        REQUIRE(role_to_atspi_role(role) != kRolePanel);
    }
}

TEST_CASE("expanded AtspiRole numbers match the published enumeration",
          "[a11y][atspi]") {
    REQUIRE(kRoleCheckBox    == 7);
    REQUIRE(kRoleComboBox    == 11);
    REQUIRE(kRoleDialog      == 16);
    REQUIRE(kRoleList        == 31);
    REQUIRE(kRoleListItem    == 32);
    REQUIRE(kRoleMenu        == 33);
    REQUIRE(kRoleMenuItem    == 35);
    REQUIRE(kRolePageTab     == 37);
    REQUIRE(kRolePageTabList == 38);
    REQUIRE(kRolePushButton  == 43);
    REQUIRE(kRoleRadioButton == 44);
    REQUIRE(kRoleScrollBar   == 48);
    REQUIRE(kRoleTable       == 55);
    REQUIRE(kRoleTableCell   == 56);
    REQUIRE(kRoleEntry       == 79);
    REQUIRE(kRoleHeading     == 83);
    REQUIRE(kRoleLink        == 88);
    REQUIRE(kRoleTableRow    == 90);
}

TEST_CASE("expanded role names match the AT-SPI role-name strings",
          "[a11y][atspi]") {
    using namespace std::string_view_literals;
    REQUIRE(role_to_atspi_role_name(View::AccessRole::button)     == "push button"sv);
    REQUIRE(role_to_atspi_role_name(View::AccessRole::link)       == "link"sv);
    REQUIRE(role_to_atspi_role_name(View::AccessRole::checkbox)   == "check box"sv);
    REQUIRE(role_to_atspi_role_name(View::AccessRole::text_field) == "entry"sv);
    REQUIRE(role_to_atspi_role_name(View::AccessRole::combo_box)  == "combo box"sv);
    REQUIRE(role_to_atspi_role_name(View::AccessRole::table)      == "table"sv);
    REQUIRE(role_to_atspi_role_name(View::AccessRole::scroll_bar) == "scroll bar"sv);
}

TEST_CASE("AT-SPI mapping is total for unknown role values", "[a11y][atspi]") {
    auto unknown = static_cast<View::AccessRole>(999);
    REQUIRE(role_to_atspi_role(unknown) == kRolePanel);
}

TEST_CASE("AT-SPI state set packs into two 32-bit words", "[a11y][atspi]") {
    StateSet s{};
    REQUIRE(s.low == 0u);
    REQUIRE(s.high == 0u);

    // Low-word bit (index < 32).
    set_state(s, kStateEnabled);  // 8
    REQUIRE(has_state(s, kStateEnabled));
    REQUIRE((s.low & (1u << 8)) != 0u);
    REQUIRE(s.high == 0u);

    set_state(s, kStateFocusable);  // 11
    set_state(s, kStateFocused);    // 12
    REQUIRE(has_state(s, kStateFocusable));
    REQUIRE(has_state(s, kStateFocused));

    // High-word bit (index >= 32) lands in the high word, offset by 32.
    set_state(s, /*index*/ 40);
    REQUIRE(has_state(s, 40));
    REQUIRE((s.high & (1u << 8)) != 0u);
}

TEST_CASE("default AT-SPI states report enabled/visible/showing/sensitive",
          "[a11y][atspi]") {
    const StateSet s = default_states();
    REQUIRE(has_state(s, kStateEnabled));
    REQUIRE(has_state(s, kStateSensitive));
    REQUIRE(has_state(s, kStateShowing));
    REQUIRE(has_state(s, kStateVisible));
    // All four are low-word states; high word stays clear.
    REQUIRE(s.high == 0u);
}

TEST_CASE("AtspiStateType ordinals match the published enumeration",
          "[a11y][atspi]") {
    // The state bit INDICES are the ordinals of AtspiStateType in
    // at-spi2-core's atspi-constants.h (INVALID = 0, ACTIVE = 1, ...). Getting
    // one wrong does not fail loudly — it sets a DIFFERENT state, and Orca acts
    // on it. kStateShowing / kStateVisible carried 26 / 28, which are
    // ATSPI_STATE_SINGLE_LINE and ATSPI_STATE_TRANSIENT: every Pulp accessible
    // told the screen reader it was neither showing nor visible.
    REQUIRE(kStateChecked       == 4u);
    REQUIRE(kStateEditable      == 7u);
    REQUIRE(kStateEnabled       == 8u);
    REQUIRE(kStateFocusable     == 11u);
    REQUIRE(kStateFocused       == 12u);
    REQUIRE(kStatePressed       == 20u);
    REQUIRE(kStateSensitive     == 24u);
    REQUIRE(kStateShowing       == 25u);
    REQUIRE(kStateVisible       == 30u);
    REQUIRE(kStateIndeterminate == 32u);
    REQUIRE(kStateCheckable     == 41u);

    // SHOWING and VISIBLE are adjacent to SENSITIVE / MANAGES_DESCENDANTS in the
    // enumeration; the wrong values are the neighbours that used to be set.
    REQUIRE(kStateShowing == kStateSensitive + 1u);   // ...SENSITIVE, SHOWING
    REQUIRE(kStateIndeterminate == kStateVisible + 2u);  // VISIBLE, MANAGES_DESC, INDET
}

TEST_CASE("check/press state lands in the AT-SPI state bitfield",
          "[a11y][atspi]") {
    // AT-SPI has no "value" for a checkbox: the STATE bits are the announcement.
    // accessibility_linux.cpp folds accessibility_toggle_state() in through this
    // pure helper, so the bits it will emit are pinned here — that TU never
    // compiles on the macOS gate.
    const StateSet base = default_states();

    const StateSet unchecked =
        with_toggle_state(base, AccessToggleState::off, /*from_checked=*/true);
    REQUIRE(has_state(unchecked, kStateCheckable));
    REQUIRE_FALSE(has_state(unchecked, kStateChecked));

    const StateSet checked =
        with_toggle_state(base, AccessToggleState::on, /*from_checked=*/true);
    REQUIRE(has_state(checked, kStateCheckable));
    REQUIRE(has_state(checked, kStateChecked));

    const StateSet mixed =
        with_toggle_state(base, AccessToggleState::mixed, /*from_checked=*/true);
    REQUIRE(has_state(mixed, kStateIndeterminate));
    REQUIRE_FALSE(has_state(mixed, kStateChecked));

    // aria-pressed → PRESSED, not CHECKED (and no CHECKABLE: a toggle button is
    // not a checkbox).
    const StateSet pressed =
        with_toggle_state(base, AccessToggleState::on, /*from_checked=*/false);
    REQUIRE(has_state(pressed, kStatePressed));
    REQUIRE_FALSE(has_state(pressed, kStateChecked));
    REQUIRE_FALSE(has_state(pressed, kStateCheckable));

    // Everything that is not a checkbox / switch / toggle button is untouched.
    const StateSet none =
        with_toggle_state(base, AccessToggleState::unset, /*from_checked=*/false);
    REQUIRE(none.low == base.low);
    REQUIRE(none.high == base.high);

    // EDITABLE — Orca enters text-entry mode on it.
    REQUIRE(has_state(with_editable(base, true), kStateEditable));
    REQUIRE_FALSE(has_state(with_editable(base, false), kStateEditable));
}
