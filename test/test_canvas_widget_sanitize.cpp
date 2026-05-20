// test_canvas_widget_sanitize.cpp — extracted from test_canvas_widget.cpp
// in the 2026-05 Phase 5 P5-3 follow-up refactor.
//
// CanvasWidget::add_command numeric-sanitization cluster (pulp #1387
// gap #2). Pins the contract that JS-supplied NaN / ±Infinity values
// land as 0 in the recorded draw command, while finite values are
// preserved exactly. Originally lived as a contiguous block inside
// test_canvas_widget.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/canvas/canvas.hpp>
#include <cmath>
#include <limits>

using namespace pulp::view;
using pulp::canvas::DrawCommand;

TEST_CASE("CanvasWidget::add_command sanitizes NaN to 0 (pulp #1387 gap #2)",
          "[canvas_widget][issue-1387]") {
    CanvasWidget cw;

    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::line_to;
    cmd.x = std::nanf("");
    cmd.y = 100.0f;
    cw.add_command(cmd);

    REQUIRE(cw.command_count() == 1);
    const auto& stored = cw.commands()[0];
    REQUIRE(stored.x == 0.0f);   // NaN -> 0
    REQUIRE(stored.y == 100.0f); // finite -> passthrough
}

TEST_CASE("CanvasWidget::add_command sanitizes ±Infinity to 0",
          "[canvas_widget][issue-1387]") {
    CanvasWidget cw;

    {
        CanvasDrawCmd cmd;
        cmd.type = CanvasDrawCmd::Type::move_to;
        cmd.x = std::numeric_limits<float>::infinity();
        cmd.y = 50.0f;
        cw.add_command(cmd);
    }
    {
        CanvasDrawCmd cmd;
        cmd.type = CanvasDrawCmd::Type::move_to;
        cmd.x = -std::numeric_limits<float>::infinity();
        cmd.y = 60.0f;
        cw.add_command(cmd);
    }

    REQUIRE(cw.command_count() == 2);
    REQUIRE(cw.commands()[0].x == 0.0f);
    REQUIRE(cw.commands()[1].x == 0.0f);
    REQUIRE(cw.commands()[0].y == 50.0f);
    REQUIRE(cw.commands()[1].y == 60.0f);
}

TEST_CASE("CanvasWidget::add_command sanitizes every coord field",
          "[canvas_widget][issue-1387]") {
    // Quad / cubic carry x2/y2/x3/y3; rect carries w/h; arc carries extra.
    // Verify all numeric coord/extra slots are sanitized in one shot.
    CanvasWidget cw;

    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::cubic_to;
    cmd.x  = std::nanf("");
    cmd.y  = std::nanf("");
    cmd.x2 = std::nanf("");
    cmd.y2 = std::nanf("");
    cmd.x3 = std::nanf("");
    cmd.y3 = std::nanf("");
    cmd.w  = std::numeric_limits<float>::infinity();
    cmd.h  = -std::numeric_limits<float>::infinity();
    cmd.extra = std::nanf("");
    cw.add_command(cmd);

    const auto& s = cw.commands()[0];
    REQUIRE(s.x == 0.0f);
    REQUIRE(s.y == 0.0f);
    REQUIRE(s.x2 == 0.0f);
    REQUIRE(s.y2 == 0.0f);
    REQUIRE(s.x3 == 0.0f);
    REQUIRE(s.y3 == 0.0f);
    REQUIRE(s.w == 0.0f);
    REQUIRE(s.h == 0.0f);
    REQUIRE(s.extra == 0.0f);
}

TEST_CASE("CanvasWidget::add_command sanitizes gradient stop positions",
          "[canvas_widget][issue-1387]") {
    // gradient_positions is the only std::vector<float> in CanvasDrawCmd.
    // A NaN stop position would make Skia's gradient shader produce
    // undefined output. Sanitize each entry.
    CanvasWidget cw;

    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::set_fill_gradient_linear;
    cmd.gradient_positions = {0.0f, std::nanf(""), 0.5f, std::numeric_limits<float>::infinity(), 1.0f};
    cw.add_command(cmd);

    const auto& positions = cw.commands()[0].gradient_positions;
    REQUIRE(positions.size() == 5);
    REQUIRE(positions[0] == 0.0f);
    REQUIRE(positions[1] == 0.0f);  // NaN -> 0
    REQUIRE(positions[2] == 0.5f);
    REQUIRE(positions[3] == 0.0f);  // +inf -> 0
    REQUIRE(positions[4] == 1.0f);
}

TEST_CASE("CanvasWidget::add_command preserves finite values exactly",
          "[canvas_widget][issue-1387]") {
    // Sanity check: finite values pass through unmodified — no precision
    // loss from the std::isfinite check. (isfinite is a pure predicate;
    // the value is returned unchanged when it passes.)
    CanvasWidget cw;

    CanvasDrawCmd cmd;
    cmd.type = CanvasDrawCmd::Type::fill_rect;
    cmd.x = 12.345f;
    cmd.y = -987.654f;
    cmd.w = 0.0f;     // valid zero
    cmd.h = 1e-6f;    // tiny finite
    cmd.extra = -0.0f; // negative zero is finite
    cw.add_command(cmd);

    const auto& s = cw.commands()[0];
    REQUIRE(s.x == 12.345f);
    REQUIRE(s.y == -987.654f);
    REQUIRE(s.w == 0.0f);
    REQUIRE(s.h == 1e-6f);
    REQUIRE(s.extra == -0.0f);
}

// ── pulp #1434 batch 7: Canvas2D shadow* sticky state ────────────────────────
//
// Verify the new CanvasDrawCmd shadow command types flush through to the
// underlying canvas. RecordingCanvas captures one DrawCommand per setter so
// tests can assert on the exact emit order — same pattern the bridge tests
// use for set_fill_color / set_line_width / set_blend_mode.
//
// The full JS-→ bridge → widget chain is covered by test_canvas2d_shim.cpp
// (which round-trips ctx.shadow* -> canvasSetShadow* bridge calls). Here we
// exercise the widget-internal dispatch (the leg that converts CanvasDrawCmd
// into Canvas:: virtual calls) directly.

