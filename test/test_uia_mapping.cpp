// Tests for the Pulp → Windows UIA mapping table.
// Pure-constant checks; the actual UIA provider lives in
// core/view/platform/win/accessibility_win.cpp and is validated per platform by
// integration tests.

#include <catch2/catch_approx.hpp>
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
          "[a11y][uia]") {
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
    REQUIRE(slider_type == kControlTypeSlider);
    REQUIRE(pats.count == 2);
    REQUIRE(pats.ids[0] == kPatternToggle);
    REQUIRE(pats.ids[1] == kPatternInvoke);
}

// ── Per-widget fragment helpers ──────────────────────────────────────────
// These pure helpers back the COM fragment provider in
// accessibility_win.cpp (IValueProvider / IRangeValueProvider selection
// and runtime-id derivation). They compile on every platform so the
// per-widget UIA logic can be unit-tested offline, the same contract the
// rest of this file pins for the mapping table.

TEST_CASE("range-value patterns gate the IRangeValueProvider fragment",
          "[a11y][uia]") {
    // Slider + meter advertise RangeValue; everything else does not.
    REQUIRE(role_supports_range_value(View::AccessRole::slider));
    REQUIRE(role_supports_range_value(View::AccessRole::meter));
    REQUIRE_FALSE(role_supports_range_value(View::AccessRole::toggle));
    REQUIRE_FALSE(role_supports_range_value(View::AccessRole::label));
    REQUIRE_FALSE(role_supports_range_value(View::AccessRole::group));
    REQUIRE_FALSE(role_supports_range_value(View::AccessRole::image));
    REQUIRE_FALSE(role_supports_range_value(View::AccessRole::none));
}

TEST_CASE("value pattern gates the IValueProvider fragment (writable range)",
          "[a11y][uia]") {
    // Slider is writable (Value + RangeValue); meter is read-only progress
    // (RangeValue only). This is the IsReadOnly discriminator.
    REQUIRE(role_supports_value(View::AccessRole::slider));
    REQUIRE_FALSE(role_supports_value(View::AccessRole::meter));
    REQUIRE_FALSE(role_supports_value(View::AccessRole::toggle));
    REQUIRE_FALSE(role_supports_value(View::AccessRole::label));
}

TEST_CASE("runtime ids are unique, stable, and append-id prefixed",
          "[a11y][uia]") {
    // First element is the documented UiaAppendRuntimeId (3); the key is
    // 1 + index so it is strictly positive and distinct per fragment.
    constexpr auto a = runtime_id_for_index(0);
    constexpr auto b = runtime_id_for_index(1);
    static_assert(a.ids[0] == kUiaAppendRuntimeId, "prefix is append-id");
    static_assert(a.ids[1] == 1, "index 0 → key 1");
    static_assert(b.ids[1] == 2, "index 1 → key 2");
    static_assert(RuntimeId::count == 2, "two-element runtime id");
    REQUIRE(a.ids[1] != b.ids[1]);  // distinct
    // Deterministic — same index always yields the same id.
    REQUIRE(runtime_id_for_index(7).ids[1] == runtime_id_for_index(7).ids[1]);
}

TEST_CASE("normalized value fraction clamps and maps to [0,1]",
          "[a11y][uia]") {
    REQUIRE(normalized_value_fraction(0.0, 0.0, 1.0) == Catch::Approx(0.0));
    REQUIRE(normalized_value_fraction(0.5, 0.0, 1.0) == Catch::Approx(0.5));
    REQUIRE(normalized_value_fraction(1.0, 0.0, 1.0) == Catch::Approx(1.0));
    // Out-of-range clamps.
    REQUIRE(normalized_value_fraction(-5.0, 0.0, 1.0) == Catch::Approx(0.0));
    REQUIRE(normalized_value_fraction(99.0, 0.0, 1.0) == Catch::Approx(1.0));
    // Non-unit range (e.g. -12 dB .. +12 dB at 0 → midpoint).
    REQUIRE(normalized_value_fraction(0.0, -12.0, 12.0) == Catch::Approx(0.5));
    // Degenerate range reports 0 instead of dividing by zero.
    REQUIRE(normalized_value_fraction(5.0, 3.0, 3.0) == Catch::Approx(0.0));
    REQUIRE(normalized_value_fraction(5.0, 10.0, 0.0) == Catch::Approx(0.0));
    // constexpr-usable.
    static_assert(normalized_value_fraction(1.0, 0.0, 2.0) == 0.5,
                  "midpoint maps to 0.5 at compile time");
}
