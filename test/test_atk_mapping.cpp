// Tests for the Pulp → Linux ATK / AT-SPI mapping table
// (workstream 04 slice 4.2a).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/platform/atk_mapping.hpp>

using namespace pulp::view;
using namespace pulp::view::atk;

TEST_CASE("role_to_atk returns stable ATK role IDs", "[a11y][atk]") {
    REQUIRE(role_to_atk(View::AccessRole::slider) == kRoleSlider);
    REQUIRE(role_to_atk(View::AccessRole::toggle) == kRoleToggleButton);
    REQUIRE(role_to_atk(View::AccessRole::label)  == kRoleLabel);
    REQUIRE(role_to_atk(View::AccessRole::group)  == kRolePanel);
    REQUIRE(role_to_atk(View::AccessRole::meter)  == kRoleProgressBar);
    REQUIRE(role_to_atk(View::AccessRole::image)  == kRoleImage);
    REQUIRE(role_to_atk(View::AccessRole::none)   == kRoleUnknown);
}

TEST_CASE("ATK mapping falls back for unknown role values",
          "[a11y][atk][coverage][phase3]") {
    auto unknown = static_cast<View::AccessRole>(999);
    REQUIRE(role_to_atk(unknown) == kRoleUnknown);

    auto flags = interfaces_for_role(unknown);
    REQUIRE((flags & kInterfaceComponent) != 0);
    REQUIRE((flags & kInterfaceValue) == 0);
    REQUIRE((flags & kInterfaceAction) == 0);
    REQUIRE((flags & kInterfaceText) == 0);
    REQUIRE((flags & kInterfaceImage) == 0);
}

TEST_CASE("ATK role IDs match documented AtkRole values",
          "[a11y][atk]") {
    // Locking the magic numbers against atk/atk.h so refactors can't
    // silently drift. Values: AtkRole enum in atk/atk.h.
    REQUIRE(kRoleImage        == 21);
    REQUIRE(kRoleLabel        == 24);
    REQUIRE(kRolePushButton   == 29);
    REQUIRE(kRoleProgressBar  == 33);
    REQUIRE(kRolePanel        == 35);
    REQUIRE(kRoleSlider       == 50);
    REQUIRE(kRoleToggleButton == 61);
    REQUIRE(kRoleUnknown      == 66);
}

TEST_CASE("every role advertises AtkComponent", "[a11y][atk]") {
    for (auto r : {View::AccessRole::none, View::AccessRole::slider,
                   View::AccessRole::toggle, View::AccessRole::label,
                   View::AccessRole::group, View::AccessRole::meter,
                   View::AccessRole::image}) {
        auto flags = interfaces_for_role(r);
        REQUIRE((flags & kInterfaceComponent) != 0);
    }
}

TEST_CASE("slider + meter advertise AtkValue", "[a11y][atk]") {
    REQUIRE((interfaces_for_role(View::AccessRole::slider) & kInterfaceValue) != 0);
    REQUIRE((interfaces_for_role(View::AccessRole::meter)  & kInterfaceValue) != 0);
    REQUIRE((interfaces_for_role(View::AccessRole::label)  & kInterfaceValue) == 0);
}

TEST_CASE("toggle advertises AtkAction", "[a11y][atk]") {
    REQUIRE((interfaces_for_role(View::AccessRole::toggle) & kInterfaceAction) != 0);
    REQUIRE((interfaces_for_role(View::AccessRole::slider) & kInterfaceAction) == 0);
}

TEST_CASE("label advertises AtkText", "[a11y][atk]") {
    REQUIRE((interfaces_for_role(View::AccessRole::label) & kInterfaceText) != 0);
}

TEST_CASE("image advertises AtkImage", "[a11y][atk]") {
    REQUIRE((interfaces_for_role(View::AccessRole::image) & kInterfaceImage) != 0);
}

TEST_CASE("mapping tables are constexpr-usable", "[a11y][atk]") {
    constexpr int32_t slider_role = role_to_atk(View::AccessRole::slider);
    static_assert(slider_role == kRoleSlider,
                  "slider must map to ATK slider at compile time");
    constexpr uint32_t slider_flags =
        interfaces_for_role(View::AccessRole::slider);
    static_assert((slider_flags & kInterfaceValue) != 0,
                  "slider must carry AtkValue at compile time");
    SUCCEED("constexpr evaluation passed");
}
