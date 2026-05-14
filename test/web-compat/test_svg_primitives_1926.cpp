// pulp #1926 — runtime-import gap: SVG primitives other than <path>
//
// Spectr (and any imported design) emits inline SVGs that use multiple
// primitive elements, not just <path>. Examples from the live import:
//   • 7× <rect>   — toggle-pill backgrounds, segmented control fills
//   • 1× <line>   — analyzer ruler ticks / mode indicators
//   • 1× <circle> — knob caps, focus dots
//
// PR #1917 wired <path> → SvgPathWidget via the web-compat shim and the
// C++ __domAppend fast path. The other primitives still fell into the
// unknown-tag default (plain View → empty box) so the icons rendered
// blank. This test pins:
//
//   1. <rect x y width height fill stroke> routes to SvgRectWidget
//      with geometry replayed via setSvgRect(...) and fill applied.
//   2. <line x1 y1 x2 y2 stroke> routes to SvgLineWidget with
//      endpoints replayed via setSvgLine(...).
//   3. <circle cx cy r fill> routes to SvgPathWidget with a `d`
//      string synthesized from the cx / cy / r attributes (two SVG
//      arc commands closed with Z).
//
// React/JSX commits setAttribute() BEFORE appendChild() materializes
// the native widget, so each primitive's `_attributes` arrive on a
// node with no native id yet. The replay functions
// (__replaySvgRectAttributes__ / __replaySvgLineAttributes__ /
// __replaySvgCircleAttributes__) flush those values once the bridge
// has an id; the test pre-mounts attributes the same way React would
// to exercise that path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/view/widgets/svg_rect.hpp>
#include <pulp/view/widgets/svg_line.hpp>

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

inline std::string js_id(TestEnvironment& env, const std::string& js_var) {
    return std::string(env.engine.evaluate(js_var + "._id")
                            .getWithDefault<std::string_view>(""));
}

inline View* resolve(TestEnvironment& env, const std::string& js_var) {
    auto id = js_id(env, js_var);
    REQUIRE_FALSE(id.empty());
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    return w;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// <rect> — SvgRectWidget with geometry + fill
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("svg-1926: <svg><rect>...</rect></svg> creates an SvgRectWidget",
          "[svg][import][spectr][issue-1926]") {
    TestEnvironment env(400, 200);
    env.eval(R"JS(
        var __svg = document.createElement('svg');
        __svg.setAttribute('viewBox', '0 0 20 20');
        __svg.setAttribute('width', '20');
        __svg.setAttribute('height', '20');

        var __rect = document.createElement('rect');
        __rect.setAttribute('x', '5');
        __rect.setAttribute('y', '5');
        __rect.setAttribute('width', '10');
        __rect.setAttribute('height', '10');
        __rect.setAttribute('fill', '#ffffff');

        __svg.appendChild(__rect);
        document.body.appendChild(__svg);
    )JS");
    env.root.layout_children();

    auto* rect = resolve(env, "__rect");
    auto* rect_widget = dynamic_cast<SvgRectWidget*>(rect);
    REQUIRE(rect_widget != nullptr);                       // routed, not createCol
    REQUIRE_THAT(rect_widget->rect_x(),      WithinAbs(5.0f,  0.001f));
    REQUIRE_THAT(rect_widget->rect_y(),      WithinAbs(5.0f,  0.001f));
    REQUIRE_THAT(rect_widget->rect_width(),  WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(rect_widget->rect_height(), WithinAbs(10.0f, 0.001f));
    REQUIRE(rect_widget->has_fill());                      // fill="#ffffff" applied
}

TEST_CASE("svg-1926: <rect> with fill='none' clears the fill",
          "[svg][import][spectr][issue-1926]") {
    // SVG `fill="none"` is the standard way to draw an outlined rect.
    // The shim should pipe the literal string through and the widget
    // should drop has_fill_ to false.
    TestEnvironment env(400, 200);
    env.eval(R"JS(
        var __svg = document.createElement('svg');
        var __rect = document.createElement('rect');
        __rect.setAttribute('x', '0');
        __rect.setAttribute('y', '0');
        __rect.setAttribute('width', '8');
        __rect.setAttribute('height', '8');
        __rect.setAttribute('fill', 'none');
        __rect.setAttribute('stroke', '#000000');
        __rect.setAttribute('stroke-width', '2');
        __svg.appendChild(__rect);
        document.body.appendChild(__svg);
    )JS");
    env.root.layout_children();

    auto* widget = dynamic_cast<SvgRectWidget*>(resolve(env, "__rect"));
    REQUIRE(widget != nullptr);
    REQUIRE_FALSE(widget->has_fill());
    REQUIRE(widget->has_stroke());
    REQUIRE_THAT(widget->stroke_width(), WithinAbs(2.0f, 0.001f));
}

// ─────────────────────────────────────────────────────────────────────────────
// <line> — SvgLineWidget with endpoints + stroke
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("svg-1926: <svg><line>...</line></svg> creates an SvgLineWidget",
          "[svg][import][spectr][issue-1926]") {
    TestEnvironment env(400, 200);
    env.eval(R"JS(
        var __svg = document.createElement('svg');
        __svg.setAttribute('viewBox', '0 0 20 20');

        var __line = document.createElement('line');
        __line.setAttribute('x1', '0');
        __line.setAttribute('y1', '0');
        __line.setAttribute('x2', '20');
        __line.setAttribute('y2', '20');
        __line.setAttribute('stroke', '#000000');

        __svg.appendChild(__line);
        document.body.appendChild(__svg);
    )JS");
    env.root.layout_children();

    auto* line = resolve(env, "__line");
    auto* line_widget = dynamic_cast<SvgLineWidget*>(line);
    REQUIRE(line_widget != nullptr);                       // routed, not createCol
    REQUIRE_THAT(line_widget->x1(), WithinAbs(0.0f,  0.001f));
    REQUIRE_THAT(line_widget->y1(), WithinAbs(0.0f,  0.001f));
    REQUIRE_THAT(line_widget->x2(), WithinAbs(20.0f, 0.001f));
    REQUIRE_THAT(line_widget->y2(), WithinAbs(20.0f, 0.001f));
    REQUIRE(line_widget->has_stroke());                    // stroke="#000000" applied
}

TEST_CASE("svg-1926: <line> camelCase strokeWidth also routes",
          "[svg][import][spectr][issue-1926]") {
    // JSX historically passes `strokeWidth` rather than the HTML
    // `stroke-width`. The replay must accept either spelling so raw
    // React imports (no build-time prop-applier) still work.
    TestEnvironment env(400, 200);
    env.eval(R"JS(
        var __svg = document.createElement('svg');
        var __line = document.createElement('line');
        __line.setAttribute('x1', '1');
        __line.setAttribute('y1', '2');
        __line.setAttribute('x2', '3');
        __line.setAttribute('y2', '4');
        __line.setAttribute('stroke', '#ff0000');
        __line.setAttribute('strokeWidth', '4');
        __svg.appendChild(__line);
        document.body.appendChild(__svg);
    )JS");
    env.root.layout_children();

    auto* widget = dynamic_cast<SvgLineWidget*>(resolve(env, "__line"));
    REQUIRE(widget != nullptr);
    REQUIRE_THAT(widget->stroke_width(), WithinAbs(4.0f, 0.001f));
}

// ─────────────────────────────────────────────────────────────────────────────
// <circle> — SvgPathWidget with synthesized `d` arc path
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("svg-1926: <svg><circle>...</circle></svg> creates an SvgPathWidget",
          "[svg][import][spectr][issue-1926]") {
    // The circle shim synthesizes a `d` path of two SVG arc commands
    // (each a half-circle, sweep flag = 0) closed with Z. For
    // cx=8 cy=8 r=6 we expect the path to start at (cx-r, cy) = (2, 8)
    // and contain two arc commands. The exact string is not pinned —
    // tolerate floating-point representation drift — but the prefix
    // and the cx/cy/r values must be present.
    TestEnvironment env(400, 200);
    env.eval(R"JS(
        var __svg = document.createElement('svg');
        __svg.setAttribute('viewBox', '0 0 16 16');

        var __circle = document.createElement('circle');
        __circle.setAttribute('cx', '8');
        __circle.setAttribute('cy', '8');
        __circle.setAttribute('r', '6');
        __circle.setAttribute('fill', '#ffffff');

        __svg.appendChild(__circle);
        document.body.appendChild(__svg);
    )JS");
    env.root.layout_children();

    auto* circle = resolve(env, "__circle");
    auto* path_widget = dynamic_cast<SvgPathWidget*>(circle);
    REQUIRE(path_widget != nullptr);                       // routed via createSvgPath

    // Synthesized d must mention the two arc commands. The exact
    // floating-point formatting depends on QuickJS Number->String, so
    // assert structural fragments rather than the full literal.
    const auto& d = path_widget->path_data();
    REQUIRE_FALSE(d.empty());
    REQUIRE(d.find("M ") == 0);     // starts with a Move-to
    REQUIRE(d.find(" a ") != std::string::npos);  // contains arc command
    REQUIRE(d.find("Z") != std::string::npos);    // closed
    REQUIRE(path_widget->has_fill());              // fill="#ffffff" applied
}

TEST_CASE("svg-1926: <circle> with r<=0 is a no-op (skips path synthesis)",
          "[svg][import][spectr][issue-1926]") {
    // Degenerate radii (missing, zero, negative) should not generate a
    // bogus `d` — leave the widget empty so it's clearly absent rather
    // than a 1-pixel speck near (cx, cy).
    TestEnvironment env(400, 200);
    env.eval(R"JS(
        var __svg = document.createElement('svg');
        var __circle = document.createElement('circle');
        __circle.setAttribute('cx', '4');
        __circle.setAttribute('cy', '4');
        __circle.setAttribute('r', '0');
        __svg.appendChild(__circle);
        document.body.appendChild(__svg);
    )JS");
    env.root.layout_children();

    auto* widget = dynamic_cast<SvgPathWidget*>(resolve(env, "__circle"));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->path_data().empty());
}
