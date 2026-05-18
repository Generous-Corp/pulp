// test_view_corner_radius.cpp — extracted from test_view.cpp in the
// 2026-05 Phase 5 (P5-3 first cut) refactor. Covers the pulp #1731
// corner-radius cluster: percent uniform + per-corner radius math,
// reflow-on-bounds-change, Panel default radius, view border stroke +
// outline honoring percent radius, box-shadow path reading the
// percent-resolved radius.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ── pulp #1731 — Panel & View paint must honor effective_corner_radius ─────
// Codex P1 #1: Panel had its own corner_radius_ field that paint() never
// read (it called View::effective_corner_radius which only sees View's
// slot). Codex P1 #2: View::paint_all used corner_radius_/corner_radii_[]
// directly, silently ignoring corner_radius_pct_ / corner_radii_pct_[].

TEST_CASE("pulp #1731 P1 — Panel default 8px corner radius actually paints",
          "[view][issue-1731][panel-radius]") {
    pulp::view::Panel p;
    p.set_bounds({0, 0, 100, 50});

    pulp::canvas::RecordingCanvas rc;
    p.paint(rc);

    // Panel always paints a filled background, so we expect at least one
    // fill_rounded_rect with the default 8.0f radius.
    bool saw_default = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 8.0f) < 1e-4f) {
            saw_default = true;
            break;
        }
    }
    REQUIRE(saw_default);
}

TEST_CASE("pulp #1731 P1 — Panel::set_corner_radius writes the View slot paint reads",
          "[view][issue-1731][panel-radius]") {
    pulp::view::Panel p;
    p.set_bounds({0, 0, 100, 50});
    p.set_corner_radius(16.0f);

    pulp::canvas::RecordingCanvas rc;
    p.paint(rc);

    bool saw_16 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 16.0f) < 1e-4f) {
            saw_16 = true;
            break;
        }
    }
    REQUIRE(saw_16);
    REQUIRE_THAT(p.corner_radius(), WithinAbs(16.0f, 1e-4f));
}

TEST_CASE("pulp #1731 P1 — View::paint_all honors percent uniform border-radius",
          "[view][issue-1731][percent-radius]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 100});
    v.set_background_color(pulp::canvas::Color::rgba8(10, 20, 30));
    v.set_border_radius_pct(50.0f);  // 50% of min(100,100) = 50px → circle

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_50 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 50.0f) < 1e-4f) {
            saw_50 = true;
            break;
        }
    }
    REQUIRE(saw_50);
}

TEST_CASE("pulp #1731 P1 — View border stroke honors percent radius",
          "[view][issue-1731][percent-radius]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 80, 80});
    v.set_border(pulp::canvas::Color::rgba8(255, 255, 255), 2.0f);
    v.set_border_radius_pct(50.0f);  // 50% of min(80,80) = 40px

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_40 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rounded_rect
                && std::abs(cmd.f[4] - 40.0f) < 1e-4f) {
            saw_40 = true;
            break;
        }
    }
    REQUIRE(saw_40);
}

TEST_CASE("pulp #1731 P1 — View outline honors percent radius",
          "[view][issue-1731][percent-radius]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 60, 60});
    v.set_border_radius_pct(50.0f);  // 50% of min(60,60) = 30px
    v.set_outline_width(2.0f);
    v.set_outline_color(pulp::canvas::Color::rgba8(255, 0, 0));
    v.set_outline_style(pulp::view::View::BorderStyle::solid);
    // Outline uses outline_offset + outline_width * 0.5f = 0 + 1 = 1 inflate.
    // Expected stroked radius = 30 + 1 = 31.

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_31 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rounded_rect
                && std::abs(cmd.f[4] - 31.0f) < 1e-4f) {
            saw_31 = true;
            break;
        }
    }
    REQUIRE(saw_31);
}

TEST_CASE("pulp #1731 P1 — px radius still wins when pct is 0 (default)",
          "[view][issue-1731][regression-guard]") {
    // Make sure the percent-resolving paint path doesn't silently regress
    // the px-only case for views that never set a percent.
    pulp::view::View v;
    v.set_bounds({0, 0, 200, 200});
    v.set_background_color(pulp::canvas::Color::rgba8(0, 0, 0));
    v.set_border_radius(12.0f);

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_12 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 12.0f) < 1e-4f) {
            saw_12 = true;
            break;
        }
    }
    REQUIRE(saw_12);
}

// ─────────────────────────────────────────────────────────────────────────────
// pulp #1731 codecov backfill — pin the effective_corner_radius* helper math
// and the per-corner percent paint integration. Pre-existing tests covered the
// uniform-pct paint sites; these add direct helper-level assertions plus
// per-corner % paint, box-shadow %, and bounds-coupling (resolved-px changes
// when min(width,height) changes) which Codecov flagged at 9.37% on #1731.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pulp #1731 cov — effective_corner_radius uniform pct math",
          "[issue-1731][rn][borderRadius][coverage]") {
    pulp::view::View v;

    SECTION("50% on 100x60 → 30 (50% × min(100,60))") {
        v.set_bounds({0, 0, 100, 60});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
    }
    SECTION("50% on 60x100 → 30 (different min dimension, same result)") {
        v.set_bounds({0, 0, 60, 100});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.effective_corner_radius(60.0f, 100.0f),
                     WithinAbs(30.0f, 1e-4f));
    }
    SECTION("50% on 200x200 → 100 (square: half the side)") {
        v.set_bounds({0, 0, 200, 200});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.effective_corner_radius(200.0f, 200.0f),
                     WithinAbs(100.0f, 1e-4f));
    }
    SECTION("25% on 100x100 → 25") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(25.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(25.0f, 1e-4f));
    }
    SECTION("0% → 0 (falls through to px slot which is 0 by default)") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(0.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(0.0f, 1e-4f));
    }
    SECTION("100% on 100x100 → 100 (full min dimension; clamped to half by "
            "paint-time path builder, but helper returns raw resolved px)") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(100.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(100.0f, 1e-4f));
    }
    SECTION("set_border_radius(px) after set_border_radius_pct(pct) "
            "clears the pct slot and returns the new px value") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.corner_radius_pct(), WithinAbs(50.0f, 1e-4f));
        v.set_border_radius(8.0f);
        REQUIRE_THAT(v.corner_radius_pct(), WithinAbs(0.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(8.0f, 1e-4f));
    }
}

TEST_CASE("pulp #1731 cov — effective_corner_radius_* per-corner pct math",
          "[issue-1731][rn][borderRadius][coverage]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 60});

    SECTION("each per-corner pct resolves against min(w,h) independently") {
        // min(100, 60) = 60 — every per-corner % multiplies that.
        v.set_corner_radius_tl_pct(50.0f);  // 30
        v.set_corner_radius_tr_pct(25.0f);  // 15
        v.set_corner_radius_bl_pct(10.0f);  // 6
        v.set_corner_radius_br_pct(75.0f);  // 45

        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(100.0f, 60.0f),
                     WithinAbs(15.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_bl(100.0f, 60.0f),
                     WithinAbs(6.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_br(100.0f, 60.0f),
                     WithinAbs(45.0f, 1e-4f));
    }
    SECTION("per-corner px wins when its pct slot is 0") {
        v.set_corner_radius_tl(10.0f);
        v.set_corner_radius_tr(20.0f);
        v.set_corner_radius_bl(30.0f);
        v.set_corner_radius_br(40.0f);

        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(10.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(100.0f, 60.0f),
                     WithinAbs(20.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_bl(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_br(100.0f, 60.0f),
                     WithinAbs(40.0f, 1e-4f));
    }
    SECTION("mixed px and pct: only the pct corner reflows on bounds change") {
        v.set_corner_radius_tl(8.0f);          // px — fixed
        v.set_corner_radius_tr_pct(50.0f);     // pct — reflows
        // 100x60 → tl=8, tr=30 (50% × 60)
        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(8.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
        // 200x200 → tl=8 (unchanged), tr=100 (50% × 200)
        REQUIRE_THAT(v.effective_corner_radius_tl(200.0f, 200.0f),
                     WithinAbs(8.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(200.0f, 200.0f),
                     WithinAbs(100.0f, 1e-4f));
    }
    SECTION("set_corner_radius_tl(px) after pct clears the pct slot") {
        v.set_corner_radius_tl_pct(50.0f);
        REQUIRE_THAT(v.corner_radius_tl_pct(), WithinAbs(50.0f, 1e-4f));
        v.set_corner_radius_tl(7.0f);
        REQUIRE_THAT(v.corner_radius_tl_pct(), WithinAbs(0.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(7.0f, 1e-4f));
    }
}

TEST_CASE("pulp #1731 cov — uniform percent reflows when bounds change",
          "[issue-1731][rn][borderRadius][coverage]") {
    // Same View, two different bounds — pct stays the same but resolved
    // px differs because min(w,h) differs.
    pulp::view::View v;
    v.set_border_radius_pct(50.0f);

    v.set_bounds({0, 0, 100, 60});
    REQUIRE_THAT(v.effective_corner_radius(100.0f, 60.0f),
                 WithinAbs(30.0f, 1e-4f));

    v.set_bounds({0, 0, 60, 100});
    REQUIRE_THAT(v.effective_corner_radius(60.0f, 100.0f),
                 WithinAbs(30.0f, 1e-4f));

    v.set_bounds({0, 0, 200, 200});
    REQUIRE_THAT(v.effective_corner_radius(200.0f, 200.0f),
                 WithinAbs(100.0f, 1e-4f));
}

TEST_CASE("pulp #1731 cov — per-corner pct paints as cubic_to with resolved px",
          "[issue-1731][rn][borderRadius][coverage]") {
    // Per-corner percent forces View::paint_all into the per-corner path
    // builder (build_per_corner_rounded_rect_path), which emits cubic_to
    // commands whose end-points encode each effective radius.
    //
    // From build_per_corner_rounded_rect_path (core/view/src/view.cpp):
    //   TR corner cubic_to ends at (w, tr)
    //
    // 100x60 view, TR = 25% → eff_tr = 0.25 * min(100,60) = 15.
    // So we expect a cubic_to whose final point is (100, 15).
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 60});
    v.set_background_color(pulp::canvas::Color::rgba8(10, 20, 30));
    v.set_corner_radius_tr_pct(25.0f);  // 15 px on 100x60

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    // Verify the per-corner path was selected (presence of begin_path +
    // fill_current_path rather than fill_rounded_rect for the bg).
    bool saw_fill_current_path = false;
    bool saw_tr_cubic_end = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_current_path) {
            saw_fill_current_path = true;
        }
        if (cmd.type == pulp::canvas::DrawCommand::Type::cubic_to) {
            // cubic_to packs (cp1x, cp1y, cp2x, cp2y, x, y) in f[0..5].
            // The TR-corner cubic ends at (w, tr) = (100, 15).
            const float ex = cmd.f[4];
            const float ey = cmd.f[5];
            if (std::abs(ex - 100.0f) < 1e-3f
                    && std::abs(ey - 15.0f) < 1e-3f) {
                saw_tr_cubic_end = true;
            }
        }
    }
    REQUIRE(saw_fill_current_path);
    REQUIRE(saw_tr_cubic_end);
}

TEST_CASE("pulp #1731 cov — box-shadow path reads the percent-resolved radius",
          "[issue-1731][rn][borderRadius][coverage]") {
    // View::paint_all draws outset box-shadow before the background,
    // and the shadow's rounded-rect corner is computed from
    // effective_corner_radius(...). A 50% pct on a 200x200 view should
    // produce a draw_box_shadow whose corner-radius slot (f[+]) equals
    // 100 (50% × 200).
    pulp::view::View v;
    v.set_bounds({0, 0, 200, 200});
    v.set_background_color(pulp::canvas::Color::rgba8(0, 0, 0));
    v.set_border_radius_pct(50.0f);  // 100 px on 200x200

    v.set_box_shadow(0.0f, 4.0f, 8.0f, 0.0f,
                     pulp::canvas::Color::rgba8(0, 0, 0, 128),
                     /*inset=*/false);

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    // draw_box_shadow packs (x,y,w,h,blur,spread...) starting at f[0..]
    // and stores the rounded-corner radius as a trailing param. The
    // call site is view.cpp:300 — `draw_box_shadow(0, 0, w, h, ox, oy,
    // blur, spread, color, inset, eff_r)`. RecordingCanvas captures
    // the radius in `floats` (the variable-length payload).
    bool saw_shadow_with_pct_radius = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type != pulp::canvas::DrawCommand::Type::draw_box_shadow)
            continue;
        // The radius value 100 should appear in the command's stored
        // params (either f[] or floats[]). Scan both.
        for (float val : cmd.f) {
            if (std::abs(val - 100.0f) < 1e-3f) {
                saw_shadow_with_pct_radius = true;
                break;
            }
        }
        if (!saw_shadow_with_pct_radius) {
            for (float val : cmd.floats) {
                if (std::abs(val - 100.0f) < 1e-3f) {
                    saw_shadow_with_pct_radius = true;
                    break;
                }
            }
        }
        if (saw_shadow_with_pct_radius) break;
    }
    REQUIRE(saw_shadow_with_pct_radius);
}

TEST_CASE("pulp #1731 cov — Panel::paint reads pct via effective_corner_radius",
          "[issue-1731][rn][borderRadius][coverage]") {
    // widgets.cpp:1774 — Panel reads effective_corner_radius for its
    // own fill_rounded_rect. Pin the pct path explicitly.
    pulp::view::Panel p;
    p.set_bounds({0, 0, 120, 80});
    // 25% × min(120, 80) = 20
    p.set_border_radius_pct(25.0f);

    pulp::canvas::RecordingCanvas rc;
    p.paint(rc);

    bool saw_20 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 20.0f) < 1e-4f) {
            saw_20 = true;
            break;
        }
    }
    REQUIRE(saw_20);
}
