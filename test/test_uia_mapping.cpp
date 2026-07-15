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
    // A toggle carries the Toggle pattern, which UIA defines on CheckBox, not
    // Button. It used to map to Button.
    REQUIRE(role_to_control_type(View::AccessRole::toggle) == kControlTypeCheckBox);
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
    // UIA_CustomControlTypeId is 50025. 50033 is UIA_PaneControlTypeId — the
    // value this constant used to hold, which made every unroled fragment
    // claim to be a Pane.
    REQUIRE(kControlTypeCustom      == 50025);
    REQUIRE(kControlTypeCustom      != 50033);

    REQUIRE(kControlTypeCheckBox    == 50002);
    REQUIRE(kControlTypeComboBox    == 50003);
    REQUIRE(kControlTypeEdit        == 50004);
    REQUIRE(kControlTypeHyperlink   == 50005);
    REQUIRE(kControlTypeListItem    == 50007);
    REQUIRE(kControlTypeList        == 50008);
    REQUIRE(kControlTypeMenu        == 50009);
    REQUIRE(kControlTypeMenuItem    == 50011);
    REQUIRE(kControlTypeRadioButton == 50013);
    REQUIRE(kControlTypeScrollBar   == 50014);
    REQUIRE(kControlTypeTab         == 50018);
    REQUIRE(kControlTypeTabItem     == 50019);
    REQUIRE(kControlTypeDataItem    == 50029);
    REQUIRE(kControlTypeWindow      == 50032);
    REQUIRE(kControlTypeTable       == 50036);
}

TEST_CASE("expanded roles map to their own UIA control types",
          "[a11y][uia]") {
    // The whole point of the expanded vocabulary: a button is a Button and a
    // dropdown is a ComboBox. Before this, TextButton reported as a toggle
    // (Button + Toggle pattern) and ComboBox reported as a Slider.
    REQUIRE(role_to_control_type(View::AccessRole::button)       == kControlTypeButton);
    REQUIRE(role_to_control_type(View::AccessRole::link)         == kControlTypeHyperlink);
    REQUIRE(role_to_control_type(View::AccessRole::checkbox)     == kControlTypeCheckBox);
    REQUIRE(role_to_control_type(View::AccessRole::radio)        == kControlTypeRadioButton);
    REQUIRE(role_to_control_type(View::AccessRole::text_field)   == kControlTypeEdit);
    REQUIRE(role_to_control_type(View::AccessRole::combo_box)    == kControlTypeComboBox);
    REQUIRE(role_to_control_type(View::AccessRole::list)         == kControlTypeList);
    REQUIRE(role_to_control_type(View::AccessRole::list_item)    == kControlTypeListItem);
    REQUIRE(role_to_control_type(View::AccessRole::table)        == kControlTypeTable);
    REQUIRE(role_to_control_type(View::AccessRole::row)          == kControlTypeDataItem);
    REQUIRE(role_to_control_type(View::AccessRole::cell)         == kControlTypeListItem);
    REQUIRE(role_to_control_type(View::AccessRole::tab)          == kControlTypeTabItem);
    REQUIRE(role_to_control_type(View::AccessRole::tab_list)     == kControlTypeTab);
    REQUIRE(role_to_control_type(View::AccessRole::menu)         == kControlTypeMenu);
    REQUIRE(role_to_control_type(View::AccessRole::menu_item)    == kControlTypeMenuItem);
    REQUIRE(role_to_control_type(View::AccessRole::progress_bar) == kControlTypeProgressBar);
    REQUIRE(role_to_control_type(View::AccessRole::dialog)       == kControlTypeWindow);
    REQUIRE(role_to_control_type(View::AccessRole::heading)      == kControlTypeText);
    REQUIRE(role_to_control_type(View::AccessRole::scroll_bar)   == kControlTypeScrollBar);

    // No expanded role may silently fall back to Custom — Custom is reserved
    // for `none`, which is excluded from the tree anyway.
    for (int i = 0; i <= static_cast<int>(View::AccessRole::scroll_bar); ++i) {
        const auto role = static_cast<View::AccessRole>(i);
        if (role == View::AccessRole::none) continue;
        INFO("role ordinal " << i);
        REQUIRE(role_to_control_type(role) != kControlTypeCustom);
    }
}

TEST_CASE("no pattern is advertised without a provider behind it",
          "[a11y][uia]") {
    // PulpFragmentProvider::GetPatternProvider (platform/win/accessibility_win
    // .cpp) returns exactly two interfaces: IRangeValueProvider and
    // IValueProvider. There is no IInvokeProvider, IToggleProvider or
    // ITextProvider anywhere in the codebase — advertising Invoke on a button,
    // Toggle on a checkbox, or Text on a label made Narrator query a pattern
    // that resolves to a null interface. Every ID the table can emit must be
    // one of the two the provider implements.
    for (int i = 0; i <= static_cast<int>(View::AccessRole::scroll_bar); ++i) {
        const auto role = static_cast<View::AccessRole>(i);
        const auto pats = patterns_for_role(role);
        INFO("role ordinal " << i);
        for (int p = 0; p < pats.count; ++p) {
            REQUIRE((pats.ids[p] == kPatternValue ||
                     pats.ids[p] == kPatternRangeValue));
        }
    }
    // The roles whose defining pattern is unimplemented advertise nothing.
    REQUIRE(patterns_for_role(View::AccessRole::button).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::link).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::menu_item).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::toggle).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::checkbox).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::radio).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::tab).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::label).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::heading).count == 0);
    REQUIRE(patterns_for_role(View::AccessRole::table).count == 0);
}

TEST_CASE("combo box no longer advertises the slider range patterns",
          "[a11y][uia]") {
    // ComboBox used to carry AccessRole::slider, so it advertised RangeValue +
    // Value and Narrator offered increment/decrement on a dropdown.
    REQUIRE_FALSE(role_supports_range_value(View::AccessRole::combo_box));
    // It DOES carry Value: the selected item is published to access_value, and
    // IValueProvider::get_Value reads it. ExpandCollapse — the pattern that
    // opens the dropdown — is still unimplemented and unadvertised.
    REQUIRE(role_supports_value(View::AccessRole::combo_box));
    REQUIRE(patterns_for_role(View::AccessRole::combo_box).count == 1);
}

TEST_CASE("text field advertises Value only (no ITextProvider exists)",
          "[a11y][uia]") {
    auto pats = patterns_for_role(View::AccessRole::text_field);
    REQUIRE(pats.count == 1);
    REQUIRE(pats.ids[0] == kPatternValue);
}

TEST_CASE("scroll bar keeps the range contract", "[a11y][uia]") {
    REQUIRE(role_supports_range_value(View::AccessRole::scroll_bar));
    REQUIRE(role_supports_value(View::AccessRole::scroll_bar));
}

TEST_CASE("slider advertises RangeValue + Value patterns",
          "[a11y][uia]") {
    auto pats = patterns_for_role(View::AccessRole::slider);
    REQUIRE(pats.count == 2);
    REQUIRE(pats.ids[0] == kPatternRangeValue);
    REQUIRE(pats.ids[1] == kPatternValue);
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

// ── Source-gated pattern availability ────────────────────────────────────
//
// The role is the ceiling; the View must actually have a value source. These
// predicates ARE the Windows provider's logic (accessibility_win.cpp calls
// nothing else), and that TU never compiles on the required macOS gate — so
// this is where the behavior is pinned.

TEST_CASE("a progress bar with no value interface exposes no RangeValue",
          "[a11y][uia]") {
    using R = View::AccessRole;
    // Before ProgressBar implemented AccessibilityValueInterface, the provider
    // advertised RangeValue anyway and get_Value / get_Minimum / get_Maximum
    // all returned 0.0 — Narrator announced 0 at any progress, in a degenerate
    // min == max == 0 range.
    REQUIRE_FALSE(exposes_range_value(R::progress_bar, /*has_value_iface=*/false));
    REQUIRE(exposes_range_value(R::progress_bar, /*has_value_iface=*/true));
    REQUIRE_FALSE(exposes_range_value(R::slider, false));
    REQUIRE(exposes_range_value(R::slider, true));
    // A role that never carries RangeValue stays false with any source.
    REQUIRE_FALSE(exposes_range_value(R::button, true));
}

TEST_CASE("a text field with no text source exposes no Value pattern",
          "[a11y][uia]") {
    using R = View::AccessRole;
    // Nothing to read → do not advertise the pattern. get_Value would hand
    // Narrator a NULL BSTR: "text field", then silence.
    REQUIRE_FALSE(exposes_value(R::text_field, false, false, false));
    // TextEditor implements AccessibilityTextInterface → Value is real.
    REQUIRE(exposes_value(R::text_field, false, /*text_iface=*/true, false));
    // ComboBox publishes the selected item into access_value.
    REQUIRE(exposes_value(R::combo_box, false, false, /*value_string=*/true));
    REQUIRE_FALSE(exposes_value(R::combo_box, false, false, false));
    // A slider's Value string comes from its value interface.
    REQUIRE(exposes_value(R::slider, /*value_iface=*/true, false, false));
    // A meter is RangeValue-only: never Value, whatever it carries.
    REQUIRE_FALSE(exposes_value(R::meter, true, true, true));
}

TEST_CASE("IsReadOnly means SetValue will succeed", "[a11y][uia]") {
    using R = View::AccessRole;
    // The contradiction this fixes: text_field advertised Value and reported
    // IsReadOnly = FALSE ("editable") while IValueProvider::SetValue
    // unconditionally returned UIA_E_NOTSUPPORTED.
    REQUIRE(is_read_only(R::text_field, false, /*editable_text=*/false));
    REQUIRE_FALSE(is_read_only(R::text_field, false, /*editable_text=*/true));
    // A read-only TextEditor (read_only = true → is_editable() false).
    REQUIRE(is_read_only(R::text_field, false, false));
    // A slider with a value interface is writable; without one it is not
    // (SetValue(double) returns UIA_E_NOTSUPPORTED with no interface).
    REQUIRE_FALSE(is_read_only(R::slider, /*value_iface=*/true, false));
    REQUIRE(is_read_only(R::slider, false, false));
    // A meter / progress bar carries a value interface but no Value pattern:
    // read-only, exactly as SetValue(double) behaves for it.
    REQUIRE(is_read_only(R::meter, true, false));
    REQUIRE(is_read_only(R::progress_bar, true, false));
    // A plain group is read-only.
    REQUIRE(is_read_only(R::group, false, false));
}

TEST_CASE("mapping tables are constexpr-usable", "[a11y][uia]") {
    // Force compile-time evaluation to catch any accidental runtime-only
    // dependency sneaking into the mapping functions.
    constexpr int slider_type = role_to_control_type(View::AccessRole::slider);
    static_assert(slider_type == kControlTypeSlider,
                  "slider must map to UIA slider at compile time");
    constexpr auto pats = patterns_for_role(View::AccessRole::slider);
    static_assert(pats.count == 2, "slider must carry RangeValue + Value");
    static_assert(!exposes_range_value(View::AccessRole::progress_bar, false),
                  "a progress bar with no value source must not advertise "
                  "RangeValue");
    SUCCEED("constexpr evaluation passed");
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
