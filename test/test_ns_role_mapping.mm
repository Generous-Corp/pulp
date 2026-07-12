// Tests for the shared macOS NSAccessibility role table
// (core/view/include/pulp/view/platform/ns_role_mapping.hpp), which backs BOTH
// the standalone window host (accessibility_mac.mm) and the plug-in editor host
// (plugin_view_host_mac.mm).
//
// This is the mapping a VoiceOver user actually hears on the required macOS CI
// gate, so it gets a real per-platform unit test rather than compile coverage.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/platform/ns_role_mapping.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>

using pulp::view::View;
using pulp::view::ns_role_for_access_role;

TEST_CASE("expanded roles map to real NSAccessibility roles", "[a11y][macos]") {
    REQUIRE(ns_role_for_access_role(View::AccessRole::button)
            == NSAccessibilityButtonRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::link)
            == NSAccessibilityLinkRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::checkbox)
            == NSAccessibilityCheckBoxRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::radio)
            == NSAccessibilityRadioButtonRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::text_field)
            == NSAccessibilityTextFieldRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::combo_box)
            == NSAccessibilityPopUpButtonRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::list)
            == NSAccessibilityListRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::table)
            == NSAccessibilityTableRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::cell)
            == NSAccessibilityCellRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::tab_list)
            == NSAccessibilityTabGroupRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::menu_item)
            == NSAccessibilityMenuItemRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::progress_bar)
            == NSAccessibilityProgressIndicatorRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::scroll_bar)
            == NSAccessibilityScrollBarRole);
}

TEST_CASE("no announceable role reports NSAccessibilityUnknownRole",
          "[a11y][macos]") {
    for (int i = 0; i <= static_cast<int>(View::AccessRole::scroll_bar); ++i) {
        const auto role = static_cast<View::AccessRole>(i);
        if (role == View::AccessRole::none) continue;  // excluded from the tree
        INFO("role ordinal " << i);
        REQUIRE_FALSE(ns_role_for_access_role(role) == NSAccessibilityUnknownRole);
    }
}

TEST_CASE("the widgets that announced the wrong thing now announce correctly",
          "[a11y][macos]") {
    // These four are the live bugs this change fixes. Each assertion is
    // "what VoiceOver says", end to end from the widget's default role.
    pulp::view::TextButton button{"Play"};
    REQUIRE(ns_role_for_access_role(button.access_role())
            == NSAccessibilityButtonRole);
    // ...and specifically NOT a checkbox, which is what AccessRole::toggle
    // mapped to.
    REQUIRE_FALSE(ns_role_for_access_role(button.access_role())
                  == NSAccessibilityCheckBoxRole);

    pulp::view::ComboBox combo;
    REQUIRE(ns_role_for_access_role(combo.access_role())
            == NSAccessibilityPopUpButtonRole);
    REQUIRE_FALSE(ns_role_for_access_role(combo.access_role())
                  == NSAccessibilitySliderRole);

    pulp::view::TextEditor editor;
    REQUIRE(ns_role_for_access_role(editor.access_role())
            == NSAccessibilityTextFieldRole);

    pulp::view::HyperlinkButton link{"Docs", "https://example.invalid"};
    REQUIRE(ns_role_for_access_role(link.access_role())
            == NSAccessibilityLinkRole);
}

TEST_CASE("meter and progress bar are distinct macOS indicators",
          "[a11y][macos]") {
    // A VU meter is a level indicator; a task progress bar is a progress
    // indicator. The two macOS hosts used to disagree about which one `meter`
    // was (window host said ProgressIndicator, plug-in host said
    // LevelIndicator); the shared table settles it.
    REQUIRE(ns_role_for_access_role(View::AccessRole::meter)
            == NSAccessibilityLevelIndicatorRole);
    REQUIRE(ns_role_for_access_role(View::AccessRole::progress_bar)
            == NSAccessibilityProgressIndicatorRole);
}
