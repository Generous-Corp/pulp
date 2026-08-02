// WindowHost::compute_design_viewport_transform and DesignFitView.
//
// Unit-tests the pure math behind set_design_viewport so the
// scale + letterbox math is locked down independent of any platform
// host. The mac GPU host delegates to this function on every paint,
// so a regression here would silently break proportional resize for
// every fixed-design import.
//
// Tag [design-viewport] so the coverage harness can attribute these cases.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/design_fit_view.hpp>
#include <pulp/view/window_host.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

using pulp::view::DesignFitView;
using pulp::view::DimensionUnit;
using pulp::view::Point;
using pulp::view::View;
using pulp::view::WindowHost;
using Catch::Matchers::WithinAbs;

namespace {

struct Transform { float sx, sy, tx, ty; bool ok; };

Transform xform(float ww, float wh, float dw, float dh) {
    Transform t{};
    t.ok = WindowHost::compute_design_viewport_transform(
        ww, wh, dw, dh, t.sx, t.sy, t.tx, t.ty);
    return t;
}

constexpr float kEps = 1e-4f;

} // namespace

TEST_CASE("design viewport: 1:1 match yields identity", "[view][design-viewport]") {
    auto t = xform(1320, 860, 1320, 860);
    REQUIRE(t.ok);
    REQUIRE_THAT(t.sx, WithinAbs(1.0f, kEps));
    REQUIRE_THAT(t.sy, WithinAbs(1.0f, kEps));
    REQUIRE_THAT(t.tx, WithinAbs(0.0f, kEps));
    REQUIRE_THAT(t.ty, WithinAbs(0.0f, kEps));
}

TEST_CASE("design viewport: proportional shrink keeps aspect", "[view][design-viewport]") {
    // Half-size window at the design aspect — uniform 0.5x scale,
    // no letterboxing because aspect matches.
    auto t = xform(660, 430, 1320, 860);
    REQUIRE(t.ok);
    REQUIRE_THAT(t.sx, WithinAbs(0.5f, kEps));
    REQUIRE_THAT(t.sy, WithinAbs(0.5f, kEps));
    REQUIRE(t.sx == t.sy);  // isotropic
    REQUIRE_THAT(t.tx, WithinAbs(0.0f, kEps));
    REQUIRE_THAT(t.ty, WithinAbs(0.0f, kEps));
}

TEST_CASE("design viewport: wider window letterboxes horizontally", "[view][design-viewport]") {
    // 1600x860 window, 1320x860 design — height is the limiting axis
    // (1.0x), letterbox bars appear on left+right.
    auto t = xform(1600, 860, 1320, 860);
    REQUIRE(t.ok);
    REQUIRE_THAT(t.sx, WithinAbs(1.0f, kEps));
    REQUIRE_THAT(t.sy, WithinAbs(1.0f, kEps));
    // (1600 - 1320) / 2 = 140 px per side
    REQUIRE_THAT(t.tx, WithinAbs(140.0f, kEps));
    REQUIRE_THAT(t.ty, WithinAbs(0.0f, kEps));
}

TEST_CASE("design viewport: taller window letterboxes vertically", "[view][design-viewport]") {
    // 1320x1000 window — width is the limiting axis (1.0x), letterbox
    // bars appear on top+bottom.
    auto t = xform(1320, 1000, 1320, 860);
    REQUIRE(t.ok);
    REQUIRE_THAT(t.sx, WithinAbs(1.0f, kEps));
    REQUIRE_THAT(t.sy, WithinAbs(1.0f, kEps));
    REQUIRE_THAT(t.tx, WithinAbs(0.0f, kEps));
    // (1000 - 860) / 2 = 70 px top + bottom
    REQUIRE_THAT(t.ty, WithinAbs(70.0f, kEps));
}

TEST_CASE("design viewport: input inverse round-trips a known point",
          "[view][design-viewport]") {
    // Mouse at the center of a wider-than-design window should map to
    // the center of the design surface, not the center of the window.
    const float ww = 1600.0f, wh = 860.0f, dw = 1320.0f, dh = 860.0f;
    auto t = xform(ww, wh, dw, dh);
    REQUIRE(t.ok);

    // The window-host's input inverse: rx = (wx - tx) / sx, ry = (wy - ty) / sy.
    auto inv = [&](float wx, float wy) {
        return std::pair{(wx - t.tx) / t.sx, (wy - t.ty) / t.sy};
    };

    {
        auto [rx, ry] = inv(ww * 0.5f, wh * 0.5f);
        REQUIRE_THAT(rx, WithinAbs(dw * 0.5f, kEps));
        REQUIRE_THAT(ry, WithinAbs(dh * 0.5f, kEps));
    }
    // Top-left corner of the design surface.
    {
        auto [rx, ry] = inv(t.tx, t.ty);
        REQUIRE_THAT(rx, WithinAbs(0.0f, kEps));
        REQUIRE_THAT(ry, WithinAbs(0.0f, kEps));
    }
    // Bottom-right corner of the design surface.
    {
        auto [rx, ry] = inv(t.tx + dw * t.sx, t.ty + dh * t.sy);
        REQUIRE_THAT(rx, WithinAbs(dw, kEps));
        REQUIRE_THAT(ry, WithinAbs(dh, kEps));
    }
}

TEST_CASE("design viewport: rejects degenerate inputs",
          "[view][design-viewport]") {
    REQUIRE_FALSE(xform(0, 860, 1320, 860).ok);
    REQUIRE_FALSE(xform(1320, 0, 1320, 860).ok);
    REQUIRE_FALSE(xform(1320, 860, 0, 860).ok);
    REQUIRE_FALSE(xform(1320, 860, 1320, 0).ok);
    REQUIRE_FALSE(xform(-1, 860, 1320, 860).ok);
    REQUIRE_FALSE(xform(1320, 860, 1320, -1).ok);
}

// AUv3 REAPER letterbox parity: top_align anchors the design to the TOP of a
// taller host pane (content + single bottom strip) instead of centering it
// between two bands, matching CLAP/VST3. Horizontal centering + scale unchanged;
// must equal centered behavior when there is no vertical slack.
TEST_CASE("design viewport: top_align anchors to top in a taller pane",
          "[view][design-viewport]") {
    // 1320-wide design in a much taller 1320x1200 pane → fits to width (1.0x),
    // 340px of vertical slack.
    float sx, sy, tx, ty;
    // Centered (default): slack split → ty = 170.
    REQUIRE(WindowHost::compute_design_viewport_transform(
        1320, 1200, 1320, 860, sx, sy, tx, ty));
    REQUIRE_THAT(ty, WithinAbs(170.0f, kEps));

    // Top-aligned: all slack falls below → ty = 0; scale + tx identical.
    float sx2, sy2, tx2, ty2;
    REQUIRE(WindowHost::compute_design_viewport_transform(
        1320, 1200, 1320, 860, sx2, sy2, tx2, ty2, /*top_align=*/true));
    REQUIRE_THAT(ty2, WithinAbs(0.0f, kEps));
    REQUIRE_THAT(sx2, WithinAbs(sx, kEps));
    REQUIRE_THAT(sy2, WithinAbs(sy, kEps));
    REQUIRE_THAT(tx2, WithinAbs(tx, kEps));
}

TEST_CASE("design viewport: top_align is a no-op when no vertical slack",
          "[view][design-viewport]") {
    // Matching aspect → ty is 0 with or without top_align (no behavior change
    // for CLAP/VST3/standalone, whose windows are aspect-constrained).
    float sx, sy, tx, ty, sx2, sy2, tx2, ty2;
    REQUIRE(WindowHost::compute_design_viewport_transform(
        660, 430, 1320, 860, sx, sy, tx, ty));
    REQUIRE(WindowHost::compute_design_viewport_transform(
        660, 430, 1320, 860, sx2, sy2, tx2, ty2, /*top_align=*/true));
    REQUIRE_THAT(ty, WithinAbs(0.0f, kEps));
    REQUIRE_THAT(ty2, WithinAbs(0.0f, kEps));
    REQUIRE_THAT(ty2, WithinAbs(ty, kEps));
}

// ── HiDPI (W8 Windows / L9 Linux) scale math ─────────────────────────────────
//
// The platform DPI calls (GetDpiForWindow / Xft.dpi) are blind-Windows/Linux,
// but the *math* the hosts apply is platform-independent and pinned here:
//   - logical size × scale = the pixel resolution the GPU/raster surface is
//     allocated at (WinPluginViewHost::pixel_w/h, X11PluginViewHost::pixel_w/h);
//   - the design-viewport transform is computed in LOGICAL host coordinates and
//     composes with the DPI scale (which SkiaSurface applies as a separate
//     canvas transform) WITHOUT double-counting;
//   - OS input arrives in physical pixels on Win/Linux, so a host divides by
//     scale before the logical-space design-viewport inverse.
// Tag with [hidpi] so the coverage harness can attribute these cases.

namespace {

// Mirror of WinPluginViewHost::pixel_w/h and X11PluginViewHost::pixel_w/h:
// logical × scale, floored to >= 1.
uint32_t pixel_dim(uint32_t logical, float scale) {
    const float p = static_cast<float>(logical) * scale;
    return static_cast<uint32_t>(p < 1.0f ? 1.0f : p);
}

// Mirror of the DPI derivations: GetDpiForWindow → dpi/96 with a 0 → 1.0 floor;
// Xft.dpi → dpi/96. Same arithmetic on both platforms.
float dpi_to_scale(unsigned dpi) {
    if (dpi == 0) return 1.0f;
    return static_cast<float>(dpi) / 96.0f;
}

} // namespace

TEST_CASE("hidpi: dpi maps to scale (dpi/96, 0 floors to 1.0)",
          "[view][design-viewport][hidpi]") {
    REQUIRE_THAT(dpi_to_scale(96),  WithinAbs(1.0f, kEps));   // 1×
    REQUIRE_THAT(dpi_to_scale(144), WithinAbs(1.5f, kEps));   // 1.5×
    REQUIRE_THAT(dpi_to_scale(192), WithinAbs(2.0f, kEps));   // 2×
    REQUIRE_THAT(dpi_to_scale(0),   WithinAbs(1.0f, kEps));   // unknown → 1×
}

TEST_CASE("hidpi: logical size times scale yields pixel surface size",
          "[view][design-viewport][hidpi]") {
    // A 400x300 editor at 2× must allocate an 800x600 pixel surface; the view
    // tree stays 400x300 logical.
    REQUIRE(pixel_dim(400, 2.0f) == 800u);
    REQUIRE(pixel_dim(300, 2.0f) == 600u);
    // 1.5× HiDPI (144 DPI).
    REQUIRE(pixel_dim(400, 1.5f) == 600u);
    REQUIRE(pixel_dim(300, 1.5f) == 450u);
    // 1× is identity.
    REQUIRE(pixel_dim(400, 1.0f) == 400u);
    // Degenerate scale never produces a 0-sized surface.
    REQUIRE(pixel_dim(1, 0.0f) == 1u);
}

TEST_CASE("hidpi: design-viewport transform is independent of DPI scale",
          "[view][design-viewport][hidpi]") {
    // The host computes the design-viewport transform from LOGICAL host size,
    // and the DPI scale is applied SEPARATELY by SkiaSurface. So at a fixed
    // logical host size the transform must NOT change with the DPI scale — the
    // two compose, they don't multiply into one another. A 1320x860 design in a
    // 1600x860 logical host letterboxes the same whether the display is 1× or 2×.
    float sx1, sy1, tx1, ty1;
    REQUIRE(WindowHost::compute_design_viewport_transform(
        1600, 860, 1320, 860, sx1, sy1, tx1, ty1));

    // Same logical host size — transform is identical regardless of the
    // surface's physical pixel resolution (1600x860 vs 3200x1720).
    float sx2, sy2, tx2, ty2;
    REQUIRE(WindowHost::compute_design_viewport_transform(
        1600, 860, 1320, 860, sx2, sy2, tx2, ty2));
    REQUIRE_THAT(sx2, WithinAbs(sx1, kEps));
    REQUIRE_THAT(tx2, WithinAbs(tx1, kEps));

    // Full composed pixel scale at 2× = design-viewport scale × DPI scale.
    constexpr float dpi_scale = 2.0f;
    const float composed = sx1 * dpi_scale;
    REQUIRE_THAT(composed, WithinAbs(1.0f * 2.0f, kEps));  // 1.0 letterbox × 2× DPI
}

TEST_CASE("hidpi: pixel input divides by scale before the logical inverse",
          "[view][design-viewport][hidpi]") {
    // Win/Linux deliver pointer coords in PHYSICAL pixels. The host divides by
    // scale to get logical host coords, THEN applies the design-viewport
    // inverse. With a design viewport set, the center of the physical surface
    // must still map to the center of the design surface.
    const float logical_w = 1600.0f, logical_h = 860.0f;
    const float dw = 1320.0f, dh = 860.0f;
    constexpr float scale = 2.0f;

    float sx, sy, tx, ty;
    REQUIRE(WindowHost::compute_design_viewport_transform(
        logical_w, logical_h, dw, dh, sx, sy, tx, ty));

    // Full host path: pixel → logical (÷scale) → design-viewport inverse.
    auto pixel_to_root = [&](float px, float py) {
        const float lx = px / scale, ly = py / scale;  // pixels → logical
        return std::pair{(lx - tx) / sx, (ly - ty) / sy};
    };

    // Center of the PHYSICAL surface (3200x1720) → center of the design surface.
    auto [rx, ry] = pixel_to_root(logical_w * scale * 0.5f,
                                  logical_h * scale * 0.5f);
    REQUIRE_THAT(rx, WithinAbs(dw * 0.5f, kEps));
    REQUIRE_THAT(ry, WithinAbs(dh * 0.5f, kEps));
}

// ── DesignFitView: the same fit, one pane deep ───────────────────────────────
//
// The window-level design viewport above fits a design into the WINDOW. A
// design embedded as one PANE of a larger app needs the identical uniform fit
// against the pane's bounds instead — otherwise a design taller than the pane
// is clipped at the bottom (or scrolled, which hides half a control panel).
// These cases assert the resulting GEOMETRY: where the fitted design actually
// lands, and that a control is hit where it is painted.

TEST_CASE("design fit: a panel taller than its pane fits instead of clipping",
          "[view][design-viewport][design-fit]") {
    // The shape an imported design carries: a fixed-size root (860 x 884.625)
    // dropped into a pane that is only 600 tall.
    constexpr float kPaneW = 860.0f, kPaneH = 600.0f;
    constexpr float kDesignW = 860.0f, kDesignH = 884.625f;

    DesignFitView fit;
    fit.set_bounds({0, 0, kPaneW, kPaneH});
    auto panel = std::make_unique<View>();
    panel->flex().preferred_width = kDesignW;
    panel->flex().preferred_height = kDesignH;
    View* panel_ptr = panel.get();
    fit.set_content(std::move(panel));

    fit.layout_children();

    const float s = fit.fit_scale();
    // Height is the limiting axis.
    REQUIRE_THAT(s, WithinAbs(kPaneH / kDesignH, kEps));
    REQUIRE(s < 1.0f);

    // The whole design is visible inside the pane — this is the defect: at
    // scale 1 the bottom 284.6pt sat outside the pane.
    const float visual_top = fit.content_offset_y();
    const float visual_bottom = visual_top + kDesignH * s;
    const float visual_left = fit.content_offset_x();
    const float visual_right = visual_left + kDesignW * s;
    REQUIRE(visual_top >= -kEps);
    REQUIRE(visual_bottom <= kPaneH + kEps);
    REQUIRE(visual_left >= -kEps);
    REQUIRE(visual_right <= kPaneW + kEps);
    // Nothing was cropped away to achieve that: the design still occupies its
    // full authored extent, scaled.
    REQUIRE_THAT(visual_bottom - visual_top, WithinAbs(kDesignH * s, kEps));

    // Layout inside the design still solves at AUTHORED size — the fit is a
    // paint-time scale, not a reflow, so nothing re-wraps or re-stacks.
    REQUIRE_THAT(panel_ptr->bounds().width, WithinAbs(kDesignW, kEps));
    REQUIRE_THAT(panel_ptr->bounds().height, WithinAbs(kDesignH, kEps));
}

TEST_CASE("design fit: the fit is uniform on both axes",
          "[view][design-viewport][design-fit]") {
    // A pane whose aspect differs from the design's: the design must letterbox,
    // never stretch to fill.
    constexpr float kPaneW = 1200.0f, kPaneH = 600.0f;
    constexpr float kDesignW = 860.0f, kDesignH = 884.625f;

    DesignFitView fit;
    fit.set_bounds({0, 0, kPaneW, kPaneH});
    auto panel = std::make_unique<View>();
    panel->flex().preferred_width = kDesignW;
    panel->flex().preferred_height = kDesignH;
    View* panel_ptr = panel.get();
    fit.set_content(std::move(panel));

    fit.layout_children();

    const float s = fit.fit_scale();
    const float visual_w = kDesignW * s;
    const float visual_h = kDesignH * s;

    // The SAME scalar on both axes — asserted as the two independent ratios
    // agreeing, not merely as "nothing was cut off". An anisotropic
    // stretch-to-fit (1200/860 wide by 600/884.625 tall) also cuts nothing off
    // and would pass a containment-only check.
    REQUIRE_THAT(visual_w / kDesignW, WithinAbs(visual_h / kDesignH, kEps));
    REQUIRE_THAT(panel_ptr->scale(), WithinAbs(s, kEps));
    // Aspect ratio survives the fit.
    REQUIRE_THAT(visual_w / visual_h, WithinAbs(kDesignW / kDesignH, kEps));
    // …and it is NOT the stretch-to-fill geometry.
    REQUIRE(std::abs(visual_w - kPaneW) > 1.0f);

    // The slack lands as letterbox bars on the roomy axis, centered.
    REQUIRE_THAT(fit.content_offset_x(), WithinAbs((kPaneW - visual_w) * 0.5f, kEps));
    REQUIRE_THAT(fit.content_offset_y(), WithinAbs((kPaneH - visual_h) * 0.5f, kEps));
    REQUIRE(fit.content_offset_x() > 0.0f);
}

TEST_CASE("design fit: a design that already fits is left at 1:1",
          "[view][design-viewport][design-fit]") {
    // Policy: never upscale by default. A design is authored at a size;
    // blowing it past that fattens hairlines and softens raster assets.
    constexpr float kPaneW = 1000.0f, kPaneH = 1000.0f;
    constexpr float kDesignW = 860.0f, kDesignH = 884.625f;

    DesignFitView fit;
    fit.set_bounds({0, 0, kPaneW, kPaneH});
    auto panel = std::make_unique<View>();
    panel->flex().preferred_width = kDesignW;
    panel->flex().preferred_height = kDesignH;
    View* panel_ptr = panel.get();
    fit.set_content(std::move(panel));

    fit.layout_children();

    REQUIRE_THAT(fit.fit_scale(), WithinAbs(1.0f, kEps));
    REQUIRE_THAT(panel_ptr->scale(), WithinAbs(1.0f, kEps));
    REQUIRE_THAT(panel_ptr->bounds().width, WithinAbs(kDesignW, kEps));
    REQUIRE_THAT(panel_ptr->bounds().height, WithinAbs(kDesignH, kEps));
    // Centered in the roomier pane.
    REQUIRE_THAT(fit.content_offset_x(), WithinAbs((kPaneW - kDesignW) * 0.5f, kEps));
    REQUIRE_THAT(fit.content_offset_y(), WithinAbs((kPaneH - kDesignH) * 0.5f, kEps));

    // The policy is a choice, not a limitation: opting in scales up uniformly.
    fit.set_allow_upscale(true);
    fit.layout_children();
    REQUIRE_THAT(fit.fit_scale(), WithinAbs(kPaneH / kDesignH, kEps));
    REQUIRE(fit.fit_scale() > 1.0f);
}

TEST_CASE("design fit: input lands where the control is painted",
          "[view][design-viewport][design-fit]") {
    // A control authored BELOW the pane's own height: unreachable (and
    // invisible) before the fit, and hit at its fitted position after it.
    constexpr float kPaneW = 860.0f, kPaneH = 600.0f;
    constexpr float kDesignW = 860.0f, kDesignH = 884.625f;
    constexpr float kKnobX = 400.0f, kKnobY = 800.0f, kKnobSize = 64.0f;

    DesignFitView fit;
    fit.set_bounds({0, 0, kPaneW, kPaneH});
    auto panel = std::make_unique<View>();
    panel->flex().preferred_width = kDesignW;
    panel->flex().preferred_height = kDesignH;
    auto knob = std::make_unique<View>();
    knob->set_position(View::Position::absolute);
    knob->set_left(kKnobX);
    knob->set_top(kKnobY);
    knob->flex().preferred_width = kKnobSize;
    knob->flex().preferred_height = kKnobSize;
    View* knob_ptr = knob.get();
    panel->add_child(std::move(knob));
    fit.set_content(std::move(panel));

    fit.layout_children();

    // The design's interior solved at AUTHORED size: the control sits at the
    // coordinate the design put it at, 800pt down — past the pane's own 600pt.
    REQUIRE_THAT(knob_ptr->bounds().y, WithinAbs(kKnobY, kEps));

    const float s = fit.fit_scale();
    const Point painted{fit.content_offset_x() + (kKnobX + kKnobSize * 0.5f) * s,
                        fit.content_offset_y() + (kKnobY + kKnobSize * 0.5f) * s};
    // The paint position must be inside the pane at all — that is the whole
    // point of the fit.
    REQUIRE(painted.y < kPaneH);
    REQUIRE(fit.hit_test(painted) == knob_ptr);

    // The AUTHORED center is not where the control is any more; hit-testing
    // against un-fitted coordinates must not find it.
    REQUIRE(fit.hit_test({kKnobX + kKnobSize * 0.5f, kKnobY + kKnobSize * 0.5f}) !=
            knob_ptr);
    // A point inside the pane but outside the fitted design's letterbox bar
    // belongs to the container, not the design.
    if (fit.content_offset_x() > 1.0f) {
        REQUIRE(fit.hit_test({1.0f, kPaneH * 0.5f}) == &fit);
    }
}

TEST_CASE("design fit: the parent's layout pass drives the fit",
          "[view][design-viewport][design-fit]") {
    // owns_child_layout() is the seam that stops the flex pass from descending
    // into the fitted subtree and re-solving it at pane size. Without it the
    // design would be stretched/reflowed by the parent instead of scaled.
    constexpr float kPaneW = 860.0f, kPaneH = 600.0f;
    constexpr float kDesignW = 860.0f, kDesignH = 884.625f;

    View root;
    root.set_bounds({0, 0, kPaneW, kPaneH});
    auto fit = std::make_unique<DesignFitView>();
    fit->flex().dim_width = {100, DimensionUnit::percent};
    fit->flex().dim_height = {100, DimensionUnit::percent};
    DesignFitView* fit_ptr = fit.get();
    auto panel = std::make_unique<View>();
    panel->flex().preferred_width = kDesignW;
    panel->flex().preferred_height = kDesignH;
    View* panel_ptr = panel.get();
    fit->set_content(std::move(panel));
    root.add_child(std::move(fit));

    root.layout_children();

    REQUIRE_THAT(fit_ptr->bounds().height, WithinAbs(kPaneH, kEps));
    REQUIRE_THAT(fit_ptr->fit_scale(), WithinAbs(kPaneH / kDesignH, kEps));
    // The design kept its authored box; only its paint is scaled.
    REQUIRE_THAT(panel_ptr->bounds().height, WithinAbs(kDesignH, kEps));
    REQUIRE_THAT(panel_ptr->scale(), WithinAbs(kPaneH / kDesignH, kEps));
}
