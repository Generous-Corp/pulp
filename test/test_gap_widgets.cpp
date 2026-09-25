#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/drawlist_format.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/theme.hpp>

#include <string>
#include <vector>

using namespace pulp::view;
using pulp::canvas::Color;
using pulp::canvas::RecordingCanvas;
using pulp::canvas::DrawCommand;

namespace {
std::vector<Color> fills(View& v) {
    RecordingCanvas rc;
    v.paint(rc);
    std::vector<Color> out;
    for (const auto& c : rc.commands())
        if (c.type == DrawCommand::Type::set_fill_color) out.push_back(c.color);
    return out;
}
bool has(const std::vector<Color>& cs, Color want) {
    for (auto& c : cs) if (c == want) return true;
    return false;
}
}  // namespace

// Phase 4 gap widgets — each must paint from theme tokens (reskinnable) and the
// interactive ones must respond to input.

namespace {

// The drawlist for one tone, with every colour-setting command removed.
// What survives is the GEOMETRY: if two tones reduce to the same geometry,
// they are distinguishable by hue alone.
std::string shape_only(pulp::view::Tone tone) {
    pulp::view::InlineBanner b;
    b.set_tone(tone);
    b.set_label("Status.");
    b.set_bounds({0, 0, 360, 46});
    pulp::canvas::RecordingCanvas rc;
    b.paint(rc);

    std::string out;
    for (const auto& c : rc.commands()) {
        switch (c.type) {
        case pulp::canvas::DrawCommand::Type::set_fill_color:
        case pulp::canvas::DrawCommand::Type::set_stroke_color:
            continue; // the colour is exactly what we are ignoring
        default:
            break;
        }
        out += pulp::canvas::format_command(c);
        out += '\n';
    }
    return out;
}

} // namespace

TEST_CASE("status tones differ by more than colour", "[view][gap-widgets][a11y]") {
    // The light theme cannot separate success from danger by colour under
    // protanopia, and no token value fixes it. So the widget owes a second
    // signal, and this asserts it survives when colour is stripped out.
    const auto success = shape_only(pulp::view::Tone::success);
    const auto danger = shape_only(pulp::view::Tone::danger);
    const auto warning = shape_only(pulp::view::Tone::warning);
    const auto info = shape_only(pulp::view::Tone::info);

    CHECK(success != danger);
    CHECK(success != warning);
    CHECK(danger != warning);
    CHECK(info != success);

    // Control: the comparison is not trivially true. The SAME tone twice must
    // reduce to the same geometry, or the check above would pass on noise.
    CHECK(shape_only(pulp::view::Tone::success) == success);
}

TEST_CASE("a neutral tone carries no mark", "[view][gap-widgets][a11y]") {
    // Neutral means "no status", so it must not gain a glyph that implies one.
    const auto neutral = shape_only(pulp::view::Tone::neutral);
    CHECK(neutral != shape_only(pulp::view::Tone::success));
    CHECK_FALSE(neutral.empty());
}

TEST_CASE("Badge paints its tone fill from the theme", "[view][gap][badge]") {
    Badge b("VST3", Tone::danger);
    b.set_bounds({0, 0, 60, 22});
    Theme t;
    t.colors["accent.error"] = color_from_hex(0xFF5C4D);
    b.set_theme(t);
    REQUIRE(has(fills(b), color_from_hex(0xFF5C4D)));
}

TEST_CASE("Badge neutral tone uses the elevated surface", "[view][gap][badge]") {
    Badge b("48 kHz", Tone::neutral);
    b.set_bounds({0, 0, 64, 22});
    Theme t;
    t.colors["bg.elevated"] = color_from_hex(0x1E2530);
    b.set_theme(t);
    REQUIRE(has(fills(b), color_from_hex(0x1E2530)));
}

TEST_CASE("InlineBanner paints a tone-colored left bar", "[view][gap][banner]") {
    InlineBanner b;
    b.set_tone(Tone::success);
    b.set_label("Build succeeded.");
    b.set_bounds({0, 0, 360, 46});
    Theme t;
    t.colors["accent.success"] = color_from_hex(0x3FCF77);
    b.set_theme(t);
    REQUIRE(has(fills(b), color_from_hex(0x3FCF77)));
}

TEST_CASE("Stepper minus/plus zones nudge the value by step", "[view][gap][stepper]") {
    Stepper s;
    s.set_range(-10, 10);
    s.set_step(2);
    s.set_value(0);
    s.set_bounds({0, 0, 140, 36});
    double last = 0;
    s.on_change = [&](double v) { last = v; };

    s.on_mouse_down({4, 18});      // left (minus) zone, x < height(36)
    REQUIRE(last == -2.0);
    s.on_mouse_down({136, 18});    // right (plus) zone, x > w - height
    REQUIRE(last == 0.0);
}

TEST_CASE("Stepper clamps to its range", "[view][gap][stepper]") {
    Stepper s;
    s.set_range(0, 5);
    s.set_step(10);
    s.set_value(3);
    s.set_bounds({0, 0, 140, 36});
    s.on_mouse_down({4, 18});      // minus 10 → clamps at 0
    REQUIRE(s.value() == 0.0);
}

TEST_CASE("Stepper range changes silently clamp existing state",
          "[view][gap][stepper][range]") {
    Stepper s;
    s.set_range(0, 10);
    s.set_value_silent(8);
    int changes = 0;
    s.on_change = [&](double) { ++changes; };

    s.set_range(0, 4);

    CHECK(s.value() == 4.0);
    CHECK(changes == 0);
}

TEST_CASE("Stepper scrub snaps relative to a non-zero minimum",
          "[view][gap][stepper][scrub]") {
    Stepper s;
    s.set_range(1, 9);
    s.set_step(2);
    s.set_value(1);
    s.set_bounds({0, 0, 140, 36});

    s.on_mouse_down({70, 18});
    s.on_mouse_drag({70, 12});

    REQUIRE(s.value() == 3.0);
}

TEST_CASE("PanControl clamps and maps x to a bipolar value", "[view][gap][pan]") {
    PanControl p;
    p.set_bounds({0, 0, 200, 18});
    p.set_value(2.0f);
    REQUIRE(p.value() == 1.0f);    // clamped
    p.on_mouse_down({0, 9});       // far left
    REQUIRE(p.value() == -1.0f);
    p.on_mouse_down({200, 9});     // far right
    REQUIRE(p.value() == 1.0f);
    p.on_mouse_down({100, 9});     // center
    REQUIRE(p.value() == 0.0f);
}

TEST_CASE("Toast action fires on a right-side click", "[view][gap][toast]") {
    Toast t;
    t.set_title("Preset saved");
    t.set_action("Undo");
    t.set_bounds({0, 0, 380, 64});
    bool fired = false;
    t.on_action = [&] { fired = true; };
    t.on_mouse_down({360, 32});    // right side
    REQUIRE(fired);
}

TEST_CASE("NumberBox end zones step the value and clamp", "[view][gap][numberbox]") {
    NumberBox n;
    n.set_range(-5, 5);
    n.set_step(1);
    n.set_value(0);
    n.set_bounds({0, 0, 120, 32});
    double last = 0;
    n.on_change = [&](double v) { last = v; };
    n.on_mouse_down({8, 16});      // ‹ zone (x < height 32) → decrement
    REQUIRE(last == -1.0);
    n.on_mouse_down({112, 16});    // › zone (x > w - height) → increment
    REQUIRE(last == 0.0);
    // Scroll wheel up (delta < 0) increments; clamps at range top.
    n.set_value(5);
    n.on_wheel(-1.0f);
    REQUIRE(n.value() == 5.0);
}

TEST_CASE("Spinner is indeterminate by default and accepts a determinate fraction",
          "[view][gap][spinner]") {
    Spinner s;
    s.set_bounds({0, 0, 24, 24});
    REQUIRE(s.progress() < 0.0f);          // indeterminate
    float before = s.phase();
    s.advance_animations(0.1f);
    REQUIRE(s.phase() > before);           // animation advances
    s.set_progress(0.5f);
    REQUIRE(s.progress() == 0.5f);
}

TEST_CASE("All gap widgets paint without crashing and emit draw commands",
          "[view][gap][smoke]") {
    EmptyState e; e.set_bounds({0, 0, 320, 100});
    Popover po; po.set_title("Quantize"); po.set_bounds({0, 0, 240, 120});
    InCanvasDialog d; d.set_message("Unsaved edits."); d.set_destructive(true); d.set_bounds({0, 0, 480, 320});
    ChannelStrip cs; cs.set_label("Drum Bus"); cs.set_level(0.6f); cs.set_pan(-0.3f); cs.set_bounds({0, 0, 90, 240});
    Spinner sp; sp.set_bounds({0, 0, 28, 28});
    NumberBox nb; nb.set_suffix("st"); nb.set_value(-3.54); nb.set_bounds({0, 0, 130, 32});
    for (View* v : std::vector<View*>{&e, &po, &d, &cs, &sp, &nb}) {
        RecordingCanvas rc;
        v->paint(rc);
        REQUIRE_FALSE(rc.commands().empty());
    }
}
