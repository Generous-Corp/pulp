// Tests for the Pulp → Windows UIA mapping table
// (workstream 04 slice 4.1a). Pure-constant checks — the actual UIA
// provider lives in core/view/platform/win/accessibility_win.cpp and
// is validated per platform by integration tests.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/platform/uia_mapping.hpp>

using namespace pulp::view;
using namespace pulp::view::uia;

TEST_CASE("role_to_control_type returns stable UIA IDs", "[a11y][uia]") {
    REQUIRE(role_to_control_type(View::AccessRole::slider) == kControlTypeSlider);
    REQUIRE(role_to_control_type(View::AccessRole::toggle) == kControlTypeButton);
    REQUIRE(role_to_control_type(View::AccessRole::label)  == kControlTypeText);
    REQUIRE(role_to_control_type(View::AccessRole::group)  == kControlTypeGroup);
    REQUIRE(role_to_control_type(View::AccessRole::meter)  == kControlTypeProgressBar);
    REQUIRE(role_to_control_type(View::AccessRole::image)  == kControlTypeImage);
    REQUIRE(role_to_control_type(View::AccessRole::none)   == kControlTypeCustom);
}

TEST_CASE("UIA mapping falls back for unknown role values",
          "[a11y][uia][coverage][phase3]") {
    auto unknown = static_cast<View::AccessRole>(999);
    REQUIRE(role_to_control_type(unknown) == kControlTypeCustom);
    REQUIRE(patterns_for_role(unknown).count == 0);
}

TEST_CASE("UIA control-type IDs match documented values",
          "[a11y][uia]") {
    // Documented in UIAutomationCore.h. Locking them here prevents a
    // future refactor from accidentally drifting one of the magic
    // numbers.
    REQUIRE(kControlTypeButton      == 50000);
    REQUIRE(kControlTypeImage       == 50006);
    REQUIRE(kControlTypeProgressBar == 50012);
    REQUIRE(kControlTypeSlider      == 50015);
    REQUIRE(kControlTypeText        == 50020);
    REQUIRE(kControlTypeGroup       == 50026);
    REQUIRE(kControlTypeCustom      == 50033);
}

TEST_CASE("slider advertises RangeValue + Value patterns",
          "[a11y][uia]") {
    auto pats = patterns_for_role(View::AccessRole::slider);
    REQUIRE(pats.count == 2);
    REQUIRE(pats.ids[0] == kPatternRangeValue);
    REQUIRE(pats.ids[1] == kPatternValue);
}

TEST_CASE("toggle advertises Toggle + Invoke patterns",
          "[a11y][uia]") {
    auto pats = patterns_for_role(View::AccessRole::toggle);
    REQUIRE(pats.count == 2);
    REQUIRE(pats.ids[0] == kPatternToggle);
    REQUIRE(pats.ids[1] == kPatternInvoke);
}

TEST_CASE("label advertises Text pattern", "[a11y][uia]") {
    auto pats = patterns_for_role(View::AccessRole::label);
    REQUIRE(pats.count == 1);
    REQUIRE(pats.ids[0] == kPatternText);
}

TEST_CASE("meter advertises RangeValue pattern", "[a11y][uia]") {
    auto pats = patterns_for_role(View::AccessRole::meter);
    REQUIRE(pats.count == 1);
    REQUIRE(pats.ids[0] == kPatternRangeValue);
}

TEST_CASE("group / image / none advertise no patterns",
          "[a11y][uia]") {
    REQUIRE(patterns_for_role(View::AccessRole::group).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::image).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::none).count == 0);
}

TEST_CASE("mapping tables are constexpr-usable", "[a11y][uia]") {
    // Force compile-time evaluation to catch any accidental runtime-only
    // dependency sneaking into the mapping functions.
    constexpr int slider_type = role_to_control_type(View::AccessRole::slider);
    static_assert(slider_type == kControlTypeSlider,
                  "slider must map to UIA slider at compile time");
    constexpr auto pats = patterns_for_role(View::AccessRole::toggle);
    static_assert(pats.count == 2, "toggle must carry 2 patterns");
    SUCCEED("constexpr evaluation passed");
}
