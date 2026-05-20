// test_widget_bridge_css_misc.cpp — extracted from test_widget_bridge.cpp
// in the 2026-05 Phase 5 P5-1 follow-up refactor.
//
// CSS-misc bridge tests — two small but coherent CSS clusters that
// together push test_widget_bridge.cpp under the 3,000-line P5-1 target:
//
//   * pulp #1434 batch 3: text-decoration longhands.
//     setTextDecorationColor / setTextDecorationLine / setTextDecorationStyle
//     each store the value on Label via WidgetBridge.
//   * pulp #1552: line-clamp + background-repeat.
//     setLineClamp pins clamp count on Label; backgroundRepeat /
//     backgroundPosition / backgroundSize round-trip through the bridge.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// ── pulp #1434 batch 3: text-decoration longhands ────────────────────────────

TEST_CASE("WidgetBridge setTextDecorationColor stores color on Label",
          "[view][bridge][issue-1434-batch-3]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createLabel('lab', 'hello', '')");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    REQUIRE_FALSE(lab->has_text_decoration_color());

    bridge.load_script("setTextDecorationColor('lab', '#ff0000')");
    REQUIRE(lab->has_text_decoration_color());
    REQUIRE(lab->text_decoration_color().r8() == 255);
    REQUIRE(lab->text_decoration_color().g8() == 0);
    REQUIRE(lab->text_decoration_color().b8() == 0);
}

TEST_CASE("WidgetBridge setTextDecorationStyle stores enum on Label",
          "[view][bridge][issue-1434-batch-3]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createLabel('lab', 'hello', '')");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    // Default is solid.
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::solid);

    bridge.load_script("setTextDecorationStyle('lab', 'dashed')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::dashed);

    bridge.load_script("setTextDecorationStyle('lab', 'wavy')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::wavy);

    bridge.load_script("setTextDecorationStyle('lab', 'dotted')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::dotted);

    bridge.load_script("setTextDecorationStyle('lab', 'double')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::double_);

    // Unknown value → solid (defensive default).
    bridge.load_script("setTextDecorationStyle('lab', 'wat')");
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::solid);
}

TEST_CASE("WidgetBridge text-decoration longhand setters preserve siblings",
          "[view][bridge][issue-1434-batch-3]") {
    // Mirrors the per-attribute border-fix from PR #1166 finding #4 —
    // setting one longhand must not clobber a previously-set sibling.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script(R"(
        createLabel('lab', 'hello', '');
        setTextDecoration('lab', 'underline');
        setTextDecorationColor('lab', '#00ff00');
        setTextDecorationStyle('lab', 'wavy');
    )");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    REQUIRE(lab->text_decoration() == Label::TextDecoration::underline);
    REQUIRE(lab->has_text_decoration_color());
    REQUIRE(lab->text_decoration_color().g8() == 255);
    REQUIRE(lab->text_decoration_style() == Label::TextDecorationStyle::wavy);
}

// ── pulp #1552: line-clamp + background-repeat ──────────────────────────────

TEST_CASE("WidgetBridge setLineClamp stores count on Label",
          "[view][bridge][issue-1552]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createLabel('lab', 'one\\ntwo\\nthree\\nfour', '')");
    auto* lab = dynamic_cast<Label*>(bridge.widget("lab"));
    REQUIRE(lab != nullptr);
    // Default: no clamp.
    REQUIRE(lab->line_clamp() == 0);
    REQUIRE_FALSE(lab->multi_line());

    // Setting a non-zero clamp also implicitly enables multi_line so the
    // paint path takes the multi-line branch (the implicit-wrap rule).
    bridge.load_script("setLineClamp('lab', 2)");
    REQUIRE(lab->line_clamp() == 2);
    REQUIRE(lab->multi_line());

    // Update the count — multi_line stays on.
    bridge.load_script("setLineClamp('lab', 5)");
    REQUIRE(lab->line_clamp() == 5);
    REQUIRE(lab->multi_line());

    // 0 clears the slot. multi_line is left as-is (the user may have
    // set it independently via setMultiLine / white-space).
    bridge.load_script("setLineClamp('lab', 0)");
    REQUIRE(lab->line_clamp() == 0);
    REQUIRE(lab->multi_line());  // sticky from the previous call

    // Negative values are clamped to 0 (defensive — JS shim parseInt
    // never emits a negative, but we don't want to trust that).
    bridge.load_script("setLineClamp('lab', -3)");
    REQUIRE(lab->line_clamp() == 0);
}

TEST_CASE("WidgetBridge setLineClamp truncates multi-line text in paint",
          "[view][bridge][issue-1552]") {
    using namespace pulp::canvas;

    Label label("alpha\nbeta\ngamma\ndelta");
    label.set_bounds({0, 0, 200, 200});
    label.set_multi_line(true);
    label.set_line_clamp(2);

    RecordingCanvas canvas;
    label.paint(canvas);

    // Verify exactly 2 fill_text commands emitted (one per visible line).
    auto fills = std::vector<DrawCommand>{};
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) fills.push_back(cmd);
    }
    REQUIRE(fills.size() == 2);
    // First line is verbatim, second line gets the U+2026 ellipsis
    // appended because source lines were dropped.
    REQUIRE(fills[0].text == "alpha");
    REQUIRE(fills[1].text == std::string("beta") + "\xe2\x80\xa6");

    // Clamp larger than the source-line count: paint emits all lines
    // and skips the ellipsis (no source lines were dropped).
    canvas.clear();
    label.set_line_clamp(10);
    label.paint(canvas);
    fills.clear();
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) fills.push_back(cmd);
    }
    REQUIRE(fills.size() == 4);
    REQUIRE(fills[3].text == "delta");  // no ellipsis
}

TEST_CASE("Label line-clamp shrinks text_h for vertical-align centering",
          "[view][bridge][issue-1552]") {
    using namespace pulp::canvas;

    // Codex P2 on PR #1573 — vertical positioning previously used the
    // *source* newline count for text_h, so a 5-line label clamped to 2
    // visible lines was offset upward as if the hidden lines still
    // occupied space. Verify the first visible line's y reflects a
    // 2-line block centered in the bounds, not a 5-line block.

    constexpr float kBoundsH = 200.0f;
    constexpr float kFontSize = 14.0f;
    const float lh = kFontSize * 1.4f;       // Label default line-height
    const float ascent = kFontSize * 0.85f;  // Label baseline offset

    // Reference: 2-line block (matches the clamped behavior we want).
    Label two_lines("alpha\nbeta");
    two_lines.set_bounds({0, 0, 200, kBoundsH});
    two_lines.set_multi_line(true);
    two_lines.set_font_size(kFontSize);
    two_lines.set_vertical_align(TextVerticalAlign::center);

    RecordingCanvas ref_canvas;
    two_lines.paint(ref_canvas);
    float ref_first_y = -1.0f;
    for (auto& cmd : ref_canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) {
            ref_first_y = cmd.f[1];
            break;
        }
    }
    REQUIRE(ref_first_y > 0.0f);

    // Subject: 5 source lines, clamp=2. The first visible line's y must
    // match the 2-line reference (i.e. clamp shrinks the centered block,
    // it does not push the visible lines off-center).
    Label clamped("one\ntwo\nthree\nfour\nfive");
    clamped.set_bounds({0, 0, 200, kBoundsH});
    clamped.set_multi_line(true);
    clamped.set_font_size(kFontSize);
    clamped.set_vertical_align(TextVerticalAlign::center);
    clamped.set_line_clamp(2);

    RecordingCanvas canvas;
    clamped.paint(canvas);

    std::vector<DrawCommand> fills;
    for (auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::fill_text) fills.push_back(cmd);
    }
    REQUIRE(fills.size() == 2);
    REQUIRE_THAT(fills[0].f[1], WithinAbs(ref_first_y, 1e-3));
    // Second visible line sits exactly one line-height below the first.
    REQUIRE_THAT(fills[1].f[1], WithinAbs(ref_first_y + lh, 1e-3));

    // Sanity: the centered y is meaningfully below the top-aligned y
    // (= ascent). This guards against regressing to the historic bug
    // where multi-line always painted from the top regardless of
    // vertical-align (would have ref_first_y == ascent).
    REQUIRE(ref_first_y > ascent + lh);  // strictly below 1-line top
}

TEST_CASE("WidgetBridge setBackgroundRepeat round-trips keyword on View",
          "[view][bridge][issue-1552]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.load_script("createCol('panel', '')");
    auto* panel = bridge.widget("panel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->background_repeat().empty());

    bridge.load_script("setBackgroundRepeat('panel', 'no-repeat')");
    REQUIRE(panel->background_repeat() == "no-repeat");

    bridge.load_script("setBackgroundRepeat('panel', 'repeat-x')");
    REQUIRE(panel->background_repeat() == "repeat-x");

    bridge.load_script("setBackgroundRepeat('panel', 'space')");
    REQUIRE(panel->background_repeat() == "space");

    bridge.load_script("setBackgroundRepeat('panel', '')");
    REQUIRE(panel->background_repeat().empty());
}

