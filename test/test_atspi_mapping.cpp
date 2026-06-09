// Tests for the Pulp → Linux AT-SPI2 mapping table
// (workstream 04 slice L7a-2). Pure-constant checks — the actual AT-SPI
// provider lives in core/view/platform/linux/accessibility_linux.cpp and
// is validated per platform (real AT stack) by VM integration tests.
//
// These lock the AtspiRole NUMBERS so a future edit can't silently regress
// to the legacy ATK AtkRole values the pre-L7 stub hard-coded (AtkRole and
// AtspiRole are distinct enumerations with different numeric values).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/platform/atspi_mapping.hpp>

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
    REQUIRE(kRoleProgressBar  == 51);
    REQUIRE(kRolePanel        == 54);
    REQUIRE(kRoleSlider       == 71);
    REQUIRE(kRoleApplication  == 75);
    REQUIRE(kRoleToggleButton == 79);

    // Guard against a regression back to the old ATK numbers (slider was 34,
    // toggle 42, label 24, panel 35, progress 33, image 21 under AtkRole).
    REQUIRE(kRoleSlider != 34u);
    REQUIRE(kRoleToggleButton != 42u);
    REQUIRE(kRoleLabel != 24u);
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
