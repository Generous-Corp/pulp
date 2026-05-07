// SvgRect + SvgLine widget tests — pulp #1416.
//
// Coverage targets:
//   * Defaults match SVG semantics (rect: fill=black, no stroke;
//     line: opaque-black stroke, width 1).
//   * paint() emits fill_rect at the configured x/y/w/h with the
//     stored color.
//   * paint() emits stroke_rect when has_stroke + width > 0.
//   * paint() is a no-op when w/h is zero or both fill+stroke disabled.
//   * stroke_line is emitted at the configured endpoints.
//   * set_stroke_width clamps negatives to zero.

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/widgets/svg_line.hpp>
#include <pulp/view/widgets/svg_rect.hpp>

using pulp::canvas::Color;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::view::SvgLineWidget;
using pulp::view::SvgRectWidget;

namespace {

const DrawCommand* find(const RecordingCanvas& rc, DrawCommand::Type t) {
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == t) return &cmd;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("SvgRectWidget defaults match SVG <rect>", "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    REQUIRE(w.has_fill());
    REQUIRE_FALSE(w.has_stroke());
    REQUIRE(w.stroke_width() == 1.0f);
    REQUIRE(w.rect_x() == 0.0f);
    REQUIRE(w.rect_y() == 0.0f);
    REQUIRE(w.rect_width() == 0.0f);
    REQUIRE(w.rect_height() == 0.0f);
}

TEST_CASE("SvgRectWidget set_rect stores geometry, clamps negative size",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_rect(10.0f, 20.0f, 50.0f, 30.0f);
    REQUIRE(w.rect_x() == 10.0f);
    REQUIRE(w.rect_y() == 20.0f);
    REQUIRE(w.rect_width() == 50.0f);
    REQUIRE(w.rect_height() == 30.0f);

    // Negative width / height clamp to 0 — paint() then short-circuits.
    w.set_rect(0.0f, 0.0f, -5.0f, -3.0f);
    REQUIRE(w.rect_width() == 0.0f);
    REQUIRE(w.rect_height() == 0.0f);
}

TEST_CASE("SvgRectWidget paint emits fill_rect with the configured color",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_rect(10.0f, 20.0f, 50.0f, 30.0f);
    w.set_fill_color(Color::rgba8(255, 0, 0));

    RecordingCanvas rc;
    w.paint(rc);

    const auto* fill = find(rc, DrawCommand::Type::fill_rect);
    REQUIRE(fill != nullptr);
    REQUIRE(fill->f[0] == 10.0f);
    REQUIRE(fill->f[1] == 20.0f);
    REQUIRE(fill->f[2] == 50.0f);
    REQUIRE(fill->f[3] == 30.0f);

    // The set_fill_color call must precede the fill_rect so the stored
    // color is the one the canvas applies — the recorded set_fill_color
    // command therefore appears earlier in the stream.
    const auto* set_color = find(rc, DrawCommand::Type::set_fill_color);
    REQUIRE(set_color != nullptr);
    REQUIRE(set_color->color.r8() == 255);
    REQUIRE(set_color->color.g8() == 0);
    REQUIRE(set_color->color.b8() == 0);
}

TEST_CASE("SvgRectWidget paint emits stroke_rect when stroke is set",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_rect(0.0f, 0.0f, 12.0f, 8.0f);
    w.clear_fill();
    w.set_stroke_color(Color::rgba8(0, 0, 255));
    w.set_stroke_width(2.0f);

    RecordingCanvas rc;
    w.paint(rc);

    const auto* stroke = find(rc, DrawCommand::Type::stroke_rect);
    REQUIRE(stroke != nullptr);
    REQUIRE(stroke->f[0] == 0.0f);
    REQUIRE(stroke->f[1] == 0.0f);
    REQUIRE(stroke->f[2] == 12.0f);
    REQUIRE(stroke->f[3] == 8.0f);

    // No fill_rect should appear.
    REQUIRE(find(rc, DrawCommand::Type::fill_rect) == nullptr);
}

TEST_CASE("SvgRectWidget paint is a no-op when geometry is zero",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_fill_color(Color::rgba8(255, 0, 0));
    // Default width/height = 0.

    RecordingCanvas rc;
    w.paint(rc);

    REQUIRE(find(rc, DrawCommand::Type::fill_rect) == nullptr);
    REQUIRE(find(rc, DrawCommand::Type::stroke_rect) == nullptr);
}

TEST_CASE("SvgRectWidget paint is a no-op when both fill and stroke disabled",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_rect(0.0f, 0.0f, 10.0f, 10.0f);
    w.clear_fill();
    // Default has_stroke_ is false.

    RecordingCanvas rc;
    w.paint(rc);
    REQUIRE(rc.commands().empty());
}

TEST_CASE("SvgRectWidget paint emits both fill and stroke when both enabled",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_rect(1.0f, 2.0f, 30.0f, 40.0f);
    w.set_fill_color(Color::rgba8(10, 20, 30));
    w.set_stroke_color(Color::rgba8(40, 50, 60));
    w.set_stroke_width(1.5f);

    RecordingCanvas rc;
    w.paint(rc);

    REQUIRE(find(rc, DrawCommand::Type::fill_rect) != nullptr);
    REQUIRE(find(rc, DrawCommand::Type::stroke_rect) != nullptr);
}

TEST_CASE("SvgRectWidget set_stroke_width clamps negatives to zero",
          "[svg_rect][issue-1416]") {
    SvgRectWidget w;
    w.set_stroke_width(-3.0f);
    REQUIRE(w.stroke_width() == 0.0f);
}

// ─────────────────────────────────────────────────────────────────────
// SvgLineWidget tests
// ─────────────────────────────────────────────────────────────────────

TEST_CASE("SvgLineWidget defaults match SVG <line>", "[svg_line][issue-1416]") {
    SvgLineWidget w;
    REQUIRE(w.has_stroke());
    REQUIRE(w.stroke_width() == 1.0f);
    REQUIRE(w.x1() == 0.0f);
    REQUIRE(w.y1() == 0.0f);
    REQUIRE(w.x2() == 0.0f);
    REQUIRE(w.y2() == 0.0f);
}

TEST_CASE("SvgLineWidget set_line stores endpoints", "[svg_line][issue-1416]") {
    SvgLineWidget w;
    w.set_line(1.0f, 2.0f, 100.0f, 200.0f);
    REQUIRE(w.x1() == 1.0f);
    REQUIRE(w.y1() == 2.0f);
    REQUIRE(w.x2() == 100.0f);
    REQUIRE(w.y2() == 200.0f);
}

TEST_CASE("SvgLineWidget paint emits stroke_line at configured endpoints",
          "[svg_line][issue-1416]") {
    SvgLineWidget w;
    w.set_line(5.0f, 10.0f, 95.0f, 90.0f);
    w.set_stroke_color(Color::rgba8(0, 255, 0));
    w.set_stroke_width(3.0f);

    RecordingCanvas rc;
    w.paint(rc);

    const auto* line = find(rc, DrawCommand::Type::stroke_line);
    REQUIRE(line != nullptr);
    REQUIRE(line->f[0] == 5.0f);
    REQUIRE(line->f[1] == 10.0f);
    REQUIRE(line->f[2] == 95.0f);
    REQUIRE(line->f[3] == 90.0f);

    const auto* set_color = find(rc, DrawCommand::Type::set_stroke_color);
    REQUIRE(set_color != nullptr);
    REQUIRE(set_color->color.r8() == 0);
    REQUIRE(set_color->color.g8() == 255);
    REQUIRE(set_color->color.b8() == 0);

    // set_line_width must precede stroke_line so the recorded width
    // applies to the segment.
    bool saw_width = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_line_width) {
            REQUIRE(cmd.f[0] == 3.0f);
            saw_width = true;
        }
    }
    REQUIRE(saw_width);
}

TEST_CASE("SvgLineWidget paint is a no-op when stroke disabled",
          "[svg_line][issue-1416]") {
    SvgLineWidget w;
    w.set_line(0.0f, 0.0f, 10.0f, 10.0f);
    w.clear_stroke();

    RecordingCanvas rc;
    w.paint(rc);
    REQUIRE(rc.commands().empty());
}

TEST_CASE("SvgLineWidget paint is a no-op when stroke width is zero",
          "[svg_line][issue-1416]") {
    SvgLineWidget w;
    w.set_line(0.0f, 0.0f, 10.0f, 10.0f);
    w.set_stroke_width(0.0f);

    RecordingCanvas rc;
    w.paint(rc);
    REQUIRE(rc.commands().empty());
}

TEST_CASE("SvgLineWidget set_stroke_width clamps negatives to zero",
          "[svg_line][issue-1416]") {
    SvgLineWidget w;
    w.set_stroke_width(-1.0f);
    REQUIRE(w.stroke_width() == 0.0f);
}
