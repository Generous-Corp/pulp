// Regression tests. Each case below pins a bug that shipped — the kind that
// renders plausibly, breaks silently, and survives review because nothing
// asserts on it.
//
//   * A button's pressed flag was declared, read by paint(), and never written
//     by anything. The pressed face had never rendered, in any build, ever.
//   * A wheel notch nudged a control's NORMALIZED value by a fixed 0.004. On any
//     control with a quantization interval, every notch snapped straight back to
//     the step it started on: thirty notches moved the value by exactly zero.
//   * A text field had no intrinsic height, so a layout engine gave it none. The
//     field collapsed to zero height inside ANY flex tree and simply vanished.
//   * The caret and selection colours resolved keys the theme did not define, so
//     all three silently fell back to a literal and no skin could touch them.
//     The selection band was worse: its alpha was overwritten AFTER resolution,
//     so a theme asking for a translucent band got whatever the field decided.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/buttons.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <cmath>

using namespace pulp::view;

TEST_CASE("a button reports its pressed state while the pointer is held",
          "[view][regression][buttons]") {
    // The flag was declared and read by paint(), but nothing ever SET it, so the
    // pressed face was unreachable. A visual test would not have caught this --
    // it renders, it just renders the wrong face.
    TextButton b("Save");
    b.set_bounds({0, 0, 80, 24});

    REQUIRE_FALSE(b.is_pressed());

    b.on_mouse_down({10.0f, 10.0f});
    REQUIRE(b.is_pressed());

    b.on_mouse_up({10.0f, 10.0f});
    REQUIRE_FALSE(b.is_pressed());
}

TEST_CASE("a button releasing its pressed state when the pointer leaves",
          "[view][regression][buttons]") {
    TextButton b("Save");
    b.set_bounds({0, 0, 80, 24});

    b.on_mouse_down({10.0f, 10.0f});
    REQUIRE(b.is_pressed());

    // Dragging off the button abandons the press; it must not stay stuck down.
    b.on_mouse_leave();
    REQUIRE_FALSE(b.is_pressed());
}

TEST_CASE("a wheel notch moves a quantized control by one interval",
          "[view][regression][knob]") {
    // The bug: the wheel nudged the NORMALIZED value by a fixed 0.004. With an
    // interval set, 0.004 of the range was smaller than one step, so the value
    // quantized straight back to where it started. Thirty notches moved it zero.
    Knob k;
    k.set_bounds({0, 0, 64, 64});
    k.set_range(0.0, 100.0, 5.0);   // 20 steps of 5
    k.set_real_value(50.0);

    // The sign convention: a negative delta scrolls the value UP, matching the
    // continuous branch. One notch is one interval, in whichever direction.
    k.on_wheel(-1.0f);
    REQUIRE_THAT(k.real_value(), Catch::Matchers::WithinAbs(55.0, 1e-9));

    k.on_wheel(1.0f);
    REQUIRE_THAT(k.real_value(), Catch::Matchers::WithinAbs(50.0, 1e-9));
}

TEST_CASE("thirty wheel notches on a quantized control actually travel",
          "[view][regression][knob]") {
    // The shape of the original bug report: repeated notches accumulated nothing.
    Knob k;
    k.set_bounds({0, 0, 64, 64});
    k.set_range(0.0, 100.0, 5.0);
    k.set_real_value(0.0);

    for (int i = 0; i < 30; ++i)
        k.on_wheel(-1.0f);

    // 30 notches x 5 = 150, clamped to the maximum. Before the fix this whole
    // loop moved the value by exactly zero.
    REQUIRE_THAT(k.real_value(), Catch::Matchers::WithinAbs(100.0, 1e-9));
}

TEST_CASE("a text field has a non-zero intrinsic height",
          "[view][regression][text-editor]") {
    // A layout engine asks a leaf for its natural size. The field answered with
    // nothing, so it was given nothing: zero height, invisible, inside any flex
    // tree. It only ever looked right because callers set explicit bounds.
    TextEditor e;
    REQUIRE(e.intrinsic_height() > 0.0f);
}

TEST_CASE("a text field's intrinsic height tracks its font size",
          "[view][regression][text-editor]") {
    TextEditor small;
    small.set_font_size(11.0f);

    TextEditor large;
    large.set_font_size(28.0f);

    REQUIRE(large.intrinsic_height() > small.intrinsic_height());
}

TEST_CASE("the caret and selection colours are real theme tokens",
          "[view][regression][theme]") {
    // All three used to resolve keys the theme did not define, so each silently
    // fell back to a literal and no skin could reach them.
    const Theme t = derive_theme(all_presets().front().dark);

    REQUIRE(t.colors.count("text.caret") == 1);
    REQUIRE(t.colors.count("text.selection") == 1);
    REQUIRE(t.colors.count("text.selected") == 1);
}

TEST_CASE("the selection band carries its alpha as authored",
          "[view][regression][theme]") {
    // The band's alpha was overwritten AFTER resolution, so a theme asking for a
    // translucent selection got whatever the field had decided. A skin must be
    // able to choose a solid band, or a barely-there one, and be obeyed.
    // Every preset must survive derivation with its band still translucent --
    // not just the one that happens to be first in the list.
    for (const auto& preset : all_presets()) {
        const Theme t = derive_theme(preset.dark);
        const auto band = t.colors.at("text.selection");
        REQUIRE(band.a > 0.0f);
        REQUIRE(band.a < 1.0f);   // the default IS translucent, and stays that way

        // And the text drawn on top of the band is a real, distinct token --
        // not the same colour as the body text, which would make a selected run
        // unreadable.
        const auto selected = t.colors.at("text.selected");
        const auto primary  = t.colors.at("text.primary");
        const bool same_as_body = (selected.r == primary.r)
                               && (selected.g == primary.g)
                               && (selected.b == primary.b);
        REQUIRE_FALSE(same_as_body);
    }
}
