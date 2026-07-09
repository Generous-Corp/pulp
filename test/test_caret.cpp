// Caret shape geometry and the solid-while-moving blink policy.
//
// The blink state machine is driven by real elapsed time, not by paint calls,
// so the same caret must blink at the same rate on a 60 Hz and a 120 Hz
// display. Two widgets previously advanced a blink counter by a hardcoded
// 1/60 inside paint(); these cases pin the contract that replaced them.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/caret.hpp>
#include <pulp/view/motion_preferences.hpp>

#include <cmath>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

// Defaults, but with round numbers so the phase arithmetic is readable:
// on for [0, 0.5), off for [0.5, 1.0), and a 0.25 s hold after movement.
CaretBlinkConfig round_config() {
    CaretBlinkConfig c;
    c.period_seconds = 1.0f;
    c.duty = 0.5f;
    c.solid_hold_seconds = 0.25f;
    return c;
}

CaretMetrics mid_text_metrics() {
    CaretMetrics m;
    m.x = 100.0f;
    m.cell_top = 10.0f;
    m.cell_height = 20.0f;
    m.baseline = 26.0f;
    m.advance = 8.0f;
    m.nominal_advance = 7.0f;
    m.stroke = 1.5f;
    return m;
}

}  // namespace

TEST_CASE("caret style names round-trip", "[view][caret]") {
    for (auto style : {CaretStyle::ibeam, CaretStyle::underline, CaretStyle::block})
        CHECK(caret_style_from_string(caret_style_to_string(style)) == style);

    // Unknown names fall back to the default rather than failing: a caret that
    // renders beats one that does not.
    CHECK(caret_style_from_string("") == CaretStyle::ibeam);
    CHECK(caret_style_from_string("underscore") == CaretStyle::ibeam);
    CHECK(caret_style_from_string("Block") == CaretStyle::ibeam);
}

TEST_CASE("caret holds solid after movement, then resumes blinking", "[view][caret]") {
    CaretBlink blink;
    blink.set_config(round_config());

    // An arrow key: solid, and stays solid for the whole hold.
    blink.keep_solid();
    REQUIRE(blink.visible());
    REQUIRE(blink.holding_solid());

    blink.advance(0.24f);
    CHECK(blink.visible());
    CHECK(blink.holding_solid());

    // Hold expires; the blink phase starts from zero, i.e. still in the "on"
    // half. This is why a caret never flickers dark the instant you stop.
    blink.advance(0.02f);
    CHECK_FALSE(blink.holding_solid());
    CHECK(blink.visible());

    // Cross into the "off" half of the period.
    blink.advance(0.51f);
    CHECK_FALSE(blink.visible());

    // ...and back on at the top of the next period.
    blink.advance(0.5f);
    CHECK(blink.visible());

    // Another arrow key mid-blink snaps it solid again, wherever it was.
    blink.advance(0.6f);
    REQUIRE_FALSE(blink.visible());
    blink.keep_solid();
    CHECK(blink.visible());
}

TEST_CASE("caret blink is frame-rate independent", "[view][caret]") {
    // The bug this replaces: `blink_ += 1.0f/60.0f` per paint, which blinks at
    // double speed on a 120 Hz display.
    CaretBlink at_60, at_120;
    at_60.set_config(round_config());
    at_120.set_config(round_config());

    // Advance both by one second of real time, at different frame rates.
    for (int i = 0; i < 60; ++i) at_60.advance(1.0f / 60.0f);
    for (int i = 0; i < 120; ++i) at_120.advance(1.0f / 120.0f);
    CHECK(at_60.visible() == at_120.visible());

    // And they must agree at every sampled point across a full period, not
    // just at the end of one.
    for (int step = 0; step < 40; ++step) {
        for (int i = 0; i < 3; ++i) at_60.advance(1.0f / 60.0f);
        for (int i = 0; i < 6; ++i) at_120.advance(1.0f / 120.0f);
        CHECK(at_60.visible() == at_120.visible());
    }
}

TEST_CASE("a long frame consumes the solid hold and carries the remainder", "[view][caret]") {
    // A stalled render or a debugger pause hands us one huge dt. It must not
    // swallow the whole hold and leave the phase at zero, nor skip the hold.
    CaretBlink blink;
    blink.set_config(round_config());  // hold 0.25, period 1.0, duty 0.5

    blink.keep_solid();
    blink.advance(0.85f);  // 0.25 hold + 0.60 into the period → past the on-half
    CHECK_FALSE(blink.holding_solid());
    CHECK_FALSE(blink.visible());

    // Non-positive and NaN deltas are ignored rather than corrupting the phase.
    const bool before = blink.visible();
    blink.advance(0.0f);
    blink.advance(-1.0f);
    blink.advance(std::nanf(""));
    CHECK(blink.visible() == before);
}

TEST_CASE("caret blink can be disabled", "[view][caret]") {
    CaretBlinkConfig c = round_config();
    c.enabled = false;
    CaretBlink blink;
    blink.set_config(c);

    // Never blinks, at any phase.
    for (int i = 0; i < 20; ++i) {
        blink.advance(0.1f);
        CHECK(blink.visible());
    }
}

TEST_CASE("caret holds still under reduced motion", "[view][caret]") {
    // A blinking caret is animated content. MotionPolicy::Off means show it
    // and stop moving.
    MotionPreferences::instance().reset_for_tests();
    CaretBlink blink;
    blink.set_config(round_config());

    blink.advance(0.7f);  // into the "off" half
    REQUIRE_FALSE(blink.visible());

    MotionPreferences::instance().set_override(MotionPolicy::Off);
    CHECK(blink.visible());

    // Reduced (but not off) still blinks — it scales durations, it does not
    // forbid animation.
    MotionPreferences::instance().set_override(MotionPolicy::Reduced);
    CHECK_FALSE(blink.visible());

    MotionPreferences::instance().reset_for_tests();
    CHECK_FALSE(blink.visible());
}

TEST_CASE("caret blink config is sanitized", "[view][caret]") {
    CaretBlinkConfig bad;
    bad.period_seconds = 0.0f;     // would divide by zero
    bad.duty = 4.0f;               // out of range
    bad.solid_hold_seconds = -1.0f;

    CaretBlink blink;
    blink.set_config(bad);
    CHECK(blink.config().period_seconds >= 0.05f);
    CHECK(blink.config().duty == 1.0f);
    CHECK(blink.config().solid_hold_seconds == 0.0f);

    // duty 1.0 → always on; duty 0 → only the (now zero) hold shows it.
    for (int i = 0; i < 10; ++i) { blink.advance(0.03f); CHECK(blink.visible()); }

    CaretBlinkConfig off_duty = round_config();
    off_duty.duty = 0.0f;
    CaretBlink never;
    never.set_config(off_duty);
    CHECK_FALSE(never.visible());
    never.keep_solid();
    CHECK(never.visible());  // the hold still wins
}

TEST_CASE("shortening the period does not strand the phase past it", "[view][caret]") {
    CaretBlink blink;
    CaretBlinkConfig slow = round_config();
    slow.period_seconds = 4.0f;
    blink.set_config(slow);
    blink.advance(3.5f);  // phase 3.5, valid under a 4 s period

    CaretBlinkConfig fast = round_config();  // period 1.0
    blink.set_config(fast);
    CHECK(blink.config().period_seconds == 1.0f);
    // phase must have been folded into [0, 1), so visible() reads the new cycle
    CHECK(blink.visible() == (std::fmod(3.5f, 1.0f) < 0.5f));
}

// ── Geometry ─────────────────────────────────────────────────────────────

TEST_CASE("every caret style anchors at the same x", "[view][caret]") {
    const auto m = mid_text_metrics();
    for (auto style : {CaretStyle::ibeam, CaretStyle::underline, CaretStyle::block})
        CHECK_THAT(caret_rect_for_style(style, m).x, WithinAbs(m.x, 1e-4));
}

TEST_CASE("caret shapes cover the right band", "[view][caret]") {
    const auto m = mid_text_metrics();

    const Rect ibeam = caret_rect_for_style(CaretStyle::ibeam, m);
    CHECK_THAT(ibeam.width, WithinAbs(m.stroke, 1e-4));
    CHECK_THAT(ibeam.y, WithinAbs(m.cell_top, 1e-4));
    CHECK_THAT(ibeam.height, WithinAbs(m.cell_height, 1e-4));

    // The underline sits on the baseline, the way a `_` glyph does — NOT at
    // the bottom of the widget's box, and never above the baseline.
    const Rect underline = caret_rect_for_style(CaretStyle::underline, m);
    CHECK(underline.y > m.baseline);
    CHECK(underline.y < m.baseline + m.cell_height * 0.5f);
    CHECK_THAT(underline.width, WithinAbs(m.advance, 1e-4));
    CHECK_THAT(underline.height, WithinAbs(m.stroke, 1e-4));

    // The block is the whole glyph cell.
    const Rect block = caret_rect_for_style(CaretStyle::block, m);
    CHECK_THAT(block.y, WithinAbs(m.cell_top, 1e-4));
    CHECK_THAT(block.height, WithinAbs(m.cell_height, 1e-4));
    CHECK_THAT(block.width, WithinAbs(m.advance, 1e-4));
}

TEST_CASE("at end of text the caret falls back to a nominal advance", "[view][caret]") {
    // There is no glyph after the caret, so advance is 0. Underline and block
    // would collapse to zero width without the fallback; the I-beam does not
    // care, since its width is the stroke.
    auto m = mid_text_metrics();
    m.advance = 0.0f;

    CHECK_THAT(caret_advance(m), WithinAbs(m.nominal_advance, 1e-4));
    CHECK_THAT(caret_rect_for_style(CaretStyle::underline, m).width,
               WithinAbs(m.nominal_advance, 1e-4));
    CHECK_THAT(caret_rect_for_style(CaretStyle::block, m).width,
               WithinAbs(m.nominal_advance, 1e-4));
    CHECK_THAT(caret_rect_for_style(CaretStyle::ibeam, m).width,
               WithinAbs(m.stroke, 1e-4));

    // Both zero: the shapes degenerate, and paint_caret must skip them rather
    // than ask the canvas to fill an empty rect.
    m.nominal_advance = 0.0f;
    CHECK(caret_rect_for_style(CaretStyle::underline, m).is_empty());
    CHECK(caret_rect_for_style(CaretStyle::block, m).is_empty());
    CHECK_FALSE(caret_rect_for_style(CaretStyle::ibeam, m).is_empty());
}

TEST_CASE("process-wide caret defaults", "[view][caret]") {
    const CaretStyle original_style = default_caret_style();
    const CaretBlinkConfig original_blink = default_caret_blink();

    CHECK(original_style == CaretStyle::ibeam);  // the documented default

    set_default_caret_style(CaretStyle::underline);
    CHECK(default_caret_style() == CaretStyle::underline);

    CaretBlinkConfig slow;
    slow.period_seconds = 2.0f;
    set_default_caret_blink(slow);
    CHECK_THAT(default_caret_blink().period_seconds, WithinAbs(2.0, 1e-4));

    // Defaults are sanitized on the way in, like a widget's own config.
    CaretBlinkConfig bogus;
    bogus.period_seconds = -1.0f;
    set_default_caret_blink(bogus);
    CHECK(default_caret_blink().period_seconds >= 0.05f);

    set_default_caret_style(original_style);
    set_default_caret_blink(original_blink);
}
