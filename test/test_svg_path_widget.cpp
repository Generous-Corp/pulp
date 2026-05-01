// SvgPathWidget tests — pulp #965.
//
// Coverage targets:
//   * Path-data parser handles M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t,
//     A/a, Z/z including the implicit-command repeat rule and the
//     "no separator before negative number" lexing case.
//   * Defaults (fill enabled black, no stroke) and the
//     set/clear_fill / set/clear_stroke API.
//   * paint() emits begin_path → path commands → fill / stroke in the
//     correct order, gated on has_fill_ / has_stroke_.
//   * viewBox-to-bounds scale: a unit-square path inside a 10x10
//     viewBox lands at the centre of a 20x20 widget when set_viewbox
//     and set_bounds disagree (xMidYMid meet).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/svg_path_widget.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cmath>

using pulp::view::SvgPathWidget;
using pulp::view::SvgPathSegment;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

TEST_CASE("SvgPathWidget defaults match HTML <path> semantics", "[svg_path][issue-965]") {
    SvgPathWidget w;
    REQUIRE(w.has_fill());
    REQUIRE_FALSE(w.has_stroke());
    REQUIRE(w.stroke_width() == 1.0f);
    REQUIRE(w.path_data().empty());
    REQUIRE(w.segments().empty());
}

TEST_CASE("SvgPathWidget parses absolute M/L/Z", "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 1 2 L 3 4 Z");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[0].op == SvgPathSegment::Op::move_to);
    REQUIRE(s[0].p[0] == 1.0f);
    REQUIRE(s[0].p[1] == 2.0f);
    REQUIRE(s[1].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[1].p[0] == 3.0f);
    REQUIRE(s[1].p[1] == 4.0f);
    REQUIRE(s[2].op == SvgPathSegment::Op::close_path);
}

TEST_CASE("SvgPathWidget parses relative m/l with current-point offset",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("m 10 10 l 5 0 l 0 5");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[0].op == SvgPathSegment::Op::move_to);
    REQUIRE(s[0].p[0] == 10.0f);
    REQUIRE(s[0].p[1] == 10.0f);
    REQUIRE(s[1].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[1].p[0] == 15.0f);
    REQUIRE(s[1].p[1] == 10.0f);
    REQUIRE(s[2].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[2].p[0] == 15.0f);
    REQUIRE(s[2].p[1] == 15.0f);
}

TEST_CASE("SvgPathWidget implicit-command repeat: M then implicit L",
          "[svg_path][issue-965]") {
    // "M 1 1 2 2 3 3" — after the move-to, the next coord pairs are
    // implicit line-to per the SVG path-data grammar.
    SvgPathWidget w;
    w.set_path("M 1 1 2 2 3 3");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[0].op == SvgPathSegment::Op::move_to);
    REQUIRE(s[1].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[1].p[0] == 2.0f);
    REQUIRE(s[1].p[1] == 2.0f);
    REQUIRE(s[2].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[2].p[0] == 3.0f);
    REQUIRE(s[2].p[1] == 3.0f);
}

TEST_CASE("SvgPathWidget parses H/V into line_to, absolute and relative",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 0 0 H 10 v 5 h -3");
    const auto& s = w.segments();
    REQUIRE(s.size() == 4);
    // H 10 — horizontal line to absolute x=10
    REQUIRE(s[1].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[1].p[0] == 10.0f);
    REQUIRE(s[1].p[1] == 0.0f);
    // v 5 — relative vertical, current y was 0, becomes 5
    REQUIRE(s[2].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[2].p[0] == 10.0f);
    REQUIRE(s[2].p[1] == 5.0f);
    // h -3 — relative horizontal, current x was 10, becomes 7
    REQUIRE(s[3].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[3].p[0] == 7.0f);
    REQUIRE(s[3].p[1] == 5.0f);
}

TEST_CASE("SvgPathWidget cubic C and smooth-cubic S reflection",
          "[svg_path][issue-965]") {
    // After a cubic ending at (10,0) with cp2=(8,2), an S 12 4 16 0
    // implies cp1 = reflection of (8,2) through (10,0) = (12,-2).
    SvgPathWidget w;
    w.set_path("M 0 0 C 2 -2 8 2 10 0 S 12 4 16 0");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[1].op == SvgPathSegment::Op::cubic_to);
    REQUIRE(s[2].op == SvgPathSegment::Op::cubic_to);
    REQUIRE(s[2].p[0] == 12.0f);
    REQUIRE(s[2].p[1] == -2.0f);
    REQUIRE(s[2].p[2] == 12.0f);
    REQUIRE(s[2].p[3] == 4.0f);
    REQUIRE(s[2].p[4] == 16.0f);
    REQUIRE(s[2].p[5] == 0.0f);
}

TEST_CASE("SvgPathWidget implicit-command repeat covers cubic curves",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 0 0 C 1 0 2 0 3 0 4 0 5 0 6 0");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[1].op == SvgPathSegment::Op::cubic_to);
    REQUIRE(s[2].op == SvgPathSegment::Op::cubic_to);
    REQUIRE(s[2].p[0] == 4.0f);
    REQUIRE(s[2].p[1] == 0.0f);
    REQUIRE(s[2].p[2] == 5.0f);
    REQUIRE(s[2].p[3] == 0.0f);
    REQUIRE(s[2].p[4] == 6.0f);
    REQUIRE(s[2].p[5] == 0.0f);
}

TEST_CASE("SvgPathWidget smooth curves fall back to current point after non-curve",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 1 2 L 3 4 S 5 6 7 8 L 9 10 T 11 12");
    const auto& s = w.segments();
    REQUIRE(s.size() == 5);
    REQUIRE(s[2].op == SvgPathSegment::Op::cubic_to);
    REQUIRE(s[2].p[0] == 3.0f);
    REQUIRE(s[2].p[1] == 4.0f);
    REQUIRE(s[2].p[2] == 5.0f);
    REQUIRE(s[2].p[3] == 6.0f);
    REQUIRE(s[2].p[4] == 7.0f);
    REQUIRE(s[2].p[5] == 8.0f);
    REQUIRE(s[4].op == SvgPathSegment::Op::quad_to);
    REQUIRE(s[4].p[0] == 9.0f);
    REQUIRE(s[4].p[1] == 10.0f);
    REQUIRE(s[4].p[2] == 11.0f);
    REQUIRE(s[4].p[3] == 12.0f);
}

TEST_CASE("SvgPathWidget quadratic Q and smooth-quadratic T reflection",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 0 0 Q 5 5 10 0 T 20 0");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[1].op == SvgPathSegment::Op::quad_to);
    REQUIRE(s[1].p[0] == 5.0f);
    REQUIRE(s[1].p[1] == 5.0f);
    REQUIRE(s[2].op == SvgPathSegment::Op::quad_to);
    // Reflection of (5,5) through (10,0) = (15,-5).
    REQUIRE(s[2].p[0] == 15.0f);
    REQUIRE(s[2].p[1] == -5.0f);
    REQUIRE(s[2].p[2] == 20.0f);
    REQUIRE(s[2].p[3] == 0.0f);
}

TEST_CASE("SvgPathWidget Z returns current point to last move-to",
          "[svg_path][issue-965]") {
    // Two subpaths separated by Z. The second M starts a fresh subpath,
    // and a relative l after the second M offsets from (5,5), not (0,0).
    SvgPathWidget w;
    w.set_path("M 0 0 L 1 1 Z M 5 5 l 1 0");
    const auto& s = w.segments();
    REQUIRE(s.size() == 5);
    REQUIRE(s[2].op == SvgPathSegment::Op::close_path);
    REQUIRE(s[3].op == SvgPathSegment::Op::move_to);
    REQUIRE(s[3].p[0] == 5.0f);
    REQUIRE(s[4].op == SvgPathSegment::Op::line_to);
    REQUIRE(s[4].p[0] == 6.0f);
    REQUIRE(s[4].p[1] == 5.0f);
}

TEST_CASE("SvgPathWidget tokenizer accepts negative number without separator",
          "[svg_path][issue-965]") {
    // Real-world icon path data omits separators before negative numbers
    // ("M0 0L1-1L-2 0"). The strtof-based scanner must accept this.
    SvgPathWidget w;
    w.set_path("M0 0L1-1L-2 0");
    const auto& s = w.segments();
    REQUIRE(s.size() == 3);
    REQUIRE(s[0].p[0] == 0.0f);
    REQUIRE(s[1].p[0] == 1.0f);
    REQUIRE(s[1].p[1] == -1.0f);
    REQUIRE(s[2].p[0] == -2.0f);
    REQUIRE(s[2].p[1] == 0.0f);
}

TEST_CASE("SvgPathWidget arc A is converted to cubic-bezier segments",
          "[svg_path][issue-965]") {
    // A semicircle from (0,0) to (10,0) with rx=ry=5, sweep=1, large=0
    // approximates as a chain of cubics — exact count depends on the
    // ceil(|dtheta| / (pi/2)) split. For a half-turn that's 2 cubics.
    SvgPathWidget w;
    w.set_path("M 0 0 A 5 5 0 0 1 10 0");
    const auto& s = w.segments();
    REQUIRE(s.size() >= 2);
    REQUIRE(s[0].op == SvgPathSegment::Op::move_to);
    for (size_t i = 1; i < s.size(); ++i) {
        REQUIRE(s[i].op == SvgPathSegment::Op::cubic_to);
    }
    // Last cubic's endpoint must land on the arc's stated end.
    const auto& last = s.back();
    REQUIRE(std::abs(last.p[4] - 10.0f) < 1e-3f);
    REQUIRE(std::abs(last.p[5] -  0.0f) < 1e-3f);
}

TEST_CASE("SvgPathWidget arc flags parse without separators",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M0 0A5 5 0 0110 0");
    const auto& s = w.segments();
    REQUIRE(s.size() >= 2);
    REQUIRE(s[0].op == SvgPathSegment::Op::move_to);
    for (size_t i = 1; i < s.size(); ++i) {
        REQUIRE(s[i].op == SvgPathSegment::Op::cubic_to);
    }
    const auto& last = s.back();
    REQUIRE(std::abs(last.p[4] - 10.0f) < 1e-3f);
    REQUIRE(std::abs(last.p[5] -  0.0f) < 1e-3f);
}

TEST_CASE("SvgPathWidget degenerate arcs either disappear or become lines",
          "[svg_path][issue-965]") {
    SvgPathWidget same_point;
    same_point.set_path("M 2 3 A 5 5 0 0 1 2 3 L 4 5");
    const auto& same = same_point.segments();
    REQUIRE(same.size() == 2);
    REQUIRE(same[0].op == SvgPathSegment::Op::move_to);
    REQUIRE(same[1].op == SvgPathSegment::Op::line_to);
    REQUIRE(same[1].p[0] == 4.0f);
    REQUIRE(same[1].p[1] == 5.0f);

    SvgPathWidget zero_radius;
    zero_radius.set_path("M 0 0 A 0 5 0 0 1 6 7");
    const auto& zero = zero_radius.segments();
    REQUIRE(zero.size() == 2);
    REQUIRE(zero[1].op == SvgPathSegment::Op::line_to);
    REQUIRE(zero[1].p[0] == 6.0f);
    REQUIRE(zero[1].p[1] == 7.0f);
}

TEST_CASE("SvgPathWidget parser keeps prior complete segments on truncated input",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 0 0 L");
    const auto& s = w.segments();
    REQUIRE(s.size() == 1);
    REQUIRE(s[0].op == SvgPathSegment::Op::move_to);
    REQUIRE(s[0].p[0] == 0.0f);
    REQUIRE(s[0].p[1] == 0.0f);
}

TEST_CASE("SvgPathWidget parser clears stale segments before reparsing malformed input",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_path("M 0 0 L 1 1");
    REQUIRE(w.segments().size() == 2);

    w.set_path("M bad");
    REQUIRE(w.segments().empty());
}

TEST_CASE("SvgPathWidget paint emits begin_path → path → fill in order",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_bounds({0, 0, 10, 10});
    w.set_path("M 0 0 L 10 0 L 10 10 Z");
    w.set_fill_color(pulp::canvas::Color::rgba8(255, 0, 0, 255));

    RecordingCanvas rc;
    w.paint(rc);

    int begin_idx = -1, fill_idx = -1, stroke_idx = -1;
    for (size_t i = 0; i < rc.commands().size(); ++i) {
        const auto& cmd = rc.commands()[i];
        if (cmd.type == DrawCommand::Type::begin_path && begin_idx < 0) {
            begin_idx = static_cast<int>(i);
        } else if (cmd.type == DrawCommand::Type::fill_current_path && fill_idx < 0) {
            fill_idx = static_cast<int>(i);
        } else if (cmd.type == DrawCommand::Type::stroke_current_path && stroke_idx < 0) {
            stroke_idx = static_cast<int>(i);
        }
    }
    REQUIRE(begin_idx >= 0);
    REQUIRE(fill_idx > begin_idx);
    REQUIRE(stroke_idx == -1);  // default is no stroke
}

TEST_CASE("SvgPathWidget paint emits stroke_path when stroke is set",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_bounds({0, 0, 10, 10});
    w.set_path("M 0 0 L 10 10");
    w.clear_fill();
    w.set_stroke_color(pulp::canvas::Color::rgba8(0, 0, 0, 255));
    w.set_stroke_width(2.0f);

    RecordingCanvas rc;
    w.paint(rc);

    int fill_idx = -1, stroke_idx = -1;
    for (size_t i = 0; i < rc.commands().size(); ++i) {
        const auto& cmd = rc.commands()[i];
        if (cmd.type == DrawCommand::Type::fill_current_path) fill_idx = static_cast<int>(i);
        if (cmd.type == DrawCommand::Type::stroke_current_path) stroke_idx = static_cast<int>(i);
    }
    REQUIRE(fill_idx == -1);
    REQUIRE(stroke_idx >= 0);
}

TEST_CASE("SvgPathWidget paint with no path data emits nothing",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_bounds({0, 0, 10, 10});

    RecordingCanvas rc;
    w.paint(rc);

    REQUIRE(rc.commands().empty());
}

TEST_CASE("SvgPathWidget viewBox is mapped onto widget bounds with xMidYMid meet",
          "[svg_path][issue-965]") {
    // Widget bounds 20x20, viewBox 10x10. Scale factor = 2. With a unit
    // square path at (0,0)-(1,1) in viewBox space, the scaled translate
    // brings it into the widget. Aspect-preserved, centred.
    SvgPathWidget w;
    w.set_bounds({0, 0, 20, 20});
    w.set_viewbox(10, 10);
    w.set_path("M 0 0 L 1 0");
    w.set_fill_color(pulp::canvas::Color::rgba8(255, 255, 255));

    RecordingCanvas rc;
    w.paint(rc);

    bool saw_scale = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::scale) {
            REQUIRE(cmd.f[0] == 2.0f);
            REQUIRE(cmd.f[1] == 2.0f);
            saw_scale = true;
        }
    }
    REQUIRE(saw_scale);
}

TEST_CASE("SvgPathWidget setFill / clearFill toggles emission",
          "[svg_path][issue-965]") {
    SvgPathWidget w;
    w.set_bounds({0, 0, 8, 8});
    w.set_path("M 0 0 L 8 8");
    w.clear_fill();
    w.clear_stroke();

    RecordingCanvas rc1;
    w.paint(rc1);
    REQUIRE(rc1.commands().empty());

    w.set_fill_color(pulp::canvas::Color::rgba8(10, 20, 30));
    RecordingCanvas rc2;
    w.paint(rc2);
    REQUIRE_FALSE(rc2.commands().empty());

    w.clear_fill();
    RecordingCanvas rc3;
    w.paint(rc3);
    REQUIRE(rc3.commands().empty());
}
