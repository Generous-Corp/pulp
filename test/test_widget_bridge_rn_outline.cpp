// test_widget_bridge_rn_outline.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1519 — RN outline cluster.
// Round-trips outlineColor / outlineOffset / outlineStyle / outlineWidth
// from the React-Native style shim through the CSS translator + bridge
// into View's outline slot. Includes the four longhand setters + the
// `outline` shorthand decomposition + OOS-tracked non-paintable
// value fallbacks (e.g. outline-style: ridge → solid).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <thread>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// ── pulp #1519 — RN outline cluster (Color/Offset/Style/Width) ────────────
//
// Outline differs from border: it doesn't take Yoga layout space and
// it paints OUTSIDE the border-box. Each setter mutates one View slot
// in isolation; Skia paint inflates the box by (offset + width/2) and
// strokes with the standard borderStyle dash plumbing.

TEST_CASE("WidgetBridge setOutlineColor / Offset / Style / Width round-trip",
          "[view][bridge][issue-1519]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createKnob('k', 0, 0, 32, 32)");
    auto* w = bridge.widget("k");
    REQUIRE(w != nullptr);
    // Defaults: outline is paint-suppressed (style=none, width=0).
    REQUIRE(w->outline_style() == View::BorderStyle::none);
    REQUIRE(w->outline_width() == 0.0f);
    REQUIRE(w->outline_offset() == 0.0f);

    bridge.load_script("setOutlineColor('k', '#ff8800')");
    REQUIRE(w->outline_color().r8() == 0xff);
    REQUIRE(w->outline_color().g8() == 0x88);
    REQUIRE(w->outline_color().b8() == 0x00);

    bridge.load_script("setOutlineOffset('k', 4.0)");
    REQUIRE_THAT(w->outline_offset(), WithinAbs(4.0f, 1e-5f));

    bridge.load_script("setOutlineWidth('k', 2.5)");
    REQUIRE_THAT(w->outline_width(), WithinAbs(2.5f, 1e-5f));

    bridge.load_script("setOutlineStyle('k', 'dashed')");
    REQUIRE(w->outline_style() == View::BorderStyle::dashed);
}

TEST_CASE("setOutlineStyle maps each keyword to the right enum",
          "[view][bridge][issue-1519]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createPanel('a', '');  setOutlineStyle('a', 'solid');
        createPanel('b', '');  setOutlineStyle('b', 'dashed');
        createPanel('c', '');  setOutlineStyle('c', 'dotted');
        createPanel('d', '');  setOutlineStyle('d', 'double');
        createPanel('e', '');  setOutlineStyle('e', 'groove');
        createPanel('f', '');  setOutlineStyle('f', 'ridge');
        createPanel('g', '');  setOutlineStyle('g', 'inset');
        createPanel('h', '');  setOutlineStyle('h', 'outset');
        createPanel('i', '');  setOutlineStyle('i', 'none');
        createPanel('j', '');  setOutlineStyle('j', 'hidden');
        createPanel('k', '');  setOutlineStyle('k', 'parchment-curl');
    )");

    REQUIRE(bridge.widget("a")->outline_style() == View::BorderStyle::solid);
    REQUIRE(bridge.widget("b")->outline_style() == View::BorderStyle::dashed);
    REQUIRE(bridge.widget("c")->outline_style() == View::BorderStyle::dotted);
    REQUIRE(bridge.widget("d")->outline_style() == View::BorderStyle::double_);
    REQUIRE(bridge.widget("e")->outline_style() == View::BorderStyle::groove);
    REQUIRE(bridge.widget("f")->outline_style() == View::BorderStyle::ridge);
    REQUIRE(bridge.widget("g")->outline_style() == View::BorderStyle::inset);
    REQUIRE(bridge.widget("h")->outline_style() == View::BorderStyle::outset);
    REQUIRE(bridge.widget("i")->outline_style() == View::BorderStyle::none);
    REQUIRE(bridge.widget("j")->outline_style() == View::BorderStyle::hidden);
    // Unknown keyword falls back to solid (mirrors setBorderStyle).
    REQUIRE(bridge.widget("k")->outline_style() == View::BorderStyle::solid);
}

TEST_CASE("outline paints AFTER border around an inflated rect",
          "[view][widget][issue-1519]") {
    // Verify: the outline stroke is geometrically OUTSIDE the border-box.
    // The recording canvas should show a stroke_rect whose origin is
    // negative (i.e. above-and-left of the view's local origin) and
    // whose size exceeds bounds_ by 2 * (offset + width/2).
    View v;
    v.set_bounds({0, 0, 100, 80});
    v.set_outline_color({0, 0xff, 0, 0xff});
    v.set_outline_offset(3.0f);
    v.set_outline_width(2.0f);
    v.set_outline_style(View::BorderStyle::solid);

    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);

    // Find the stroke_rect emitted for the outline. With no border
    // (set_border was never called), there should be exactly one
    // stroke_rect — the outline.
    int stroke_rects_seen = 0;
    float ox = 0, oy = 0, ow = 0, oh = 0;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rect) {
            stroke_rects_seen++;
            ox = cmd.f[0];
            oy = cmd.f[1];
            ow = cmd.f[2];
            oh = cmd.f[3];
        }
    }
    REQUIRE(stroke_rects_seen == 1);

    // inflate = offset + width/2 = 3 + 1 = 4
    const float inflate = 4.0f;
    REQUIRE_THAT(ox, WithinAbs(-inflate, 1e-5f));
    REQUIRE_THAT(oy, WithinAbs(-inflate, 1e-5f));
    REQUIRE_THAT(ow, WithinAbs(100.0f + 2.0f * inflate, 1e-5f));
    REQUIRE_THAT(oh, WithinAbs(80.0f + 2.0f * inflate, 1e-5f));
}

TEST_CASE("outline-style: none/hidden short-circuit the stroke",
          "[view][widget][issue-1519]") {
    for (auto s : { View::BorderStyle::none, View::BorderStyle::hidden }) {
        View v;
        v.set_bounds({0, 0, 100, 80});
        v.set_outline_color({0, 0xff, 0, 0xff});
        v.set_outline_width(2.0f);
        v.set_outline_style(s);
        pulp::canvas::RecordingCanvas canvas;
        v.paint_all(canvas);
        for (const auto& cmd : canvas.commands()) {
            REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::stroke_rect);
            REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::stroke_rounded_rect);
        }
    }
}

TEST_CASE("outline default state emits no paint (style=none, width=0)",
          "[view][widget][issue-1519]") {
    // A view with NO outline-* setters called must not emit any
    // outline-related stroke. Belt-and-braces against accidental
    // always-on outline regression.
    View v;
    v.set_bounds({0, 0, 100, 80});
    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);
    for (const auto& cmd : canvas.commands()) {
        REQUIRE(cmd.type != pulp::canvas::DrawCommand::Type::stroke_rect);
    }
}

TEST_CASE("dashed outline emits set_line_dash then resets it",
          "[view][widget][issue-1519]") {
    View v;
    v.set_bounds({0, 0, 100, 80});
    v.set_outline_color({0xff, 0, 0, 0xff});
    v.set_outline_width(2.0f);
    v.set_outline_offset(0.0f);
    v.set_outline_style(View::BorderStyle::dashed);

    pulp::canvas::RecordingCanvas canvas;
    v.paint_all(canvas);

    int set_dash_count = 0;
    bool saw_stroke = false;
    bool dash_reset_after_stroke = false;
    size_t first_intervals_count = 0;
    size_t last_intervals_count = 999;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::set_line_dash) {
            set_dash_count++;
            if (set_dash_count == 1) first_intervals_count = cmd.floats.size();
            last_intervals_count = cmd.floats.size();
            if (saw_stroke) dash_reset_after_stroke = true;
        }
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rect)
            saw_stroke = true;
    }
    REQUIRE(saw_stroke);
    REQUIRE(set_dash_count >= 2);
    REQUIRE(first_intervals_count == 2u);
    REQUIRE(last_intervals_count == 0u);  // reset to empty
    REQUIRE(dash_reset_after_stroke);
}

