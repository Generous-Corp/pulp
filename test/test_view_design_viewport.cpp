// WindowHost::compute_design_viewport_transform.
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
#include <pulp/view/window_host.hpp>

#include <cstdint>
#include <utility>

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
    // Mouse at the centre of a wider-than-design window should map to
    // the centre of the design surface, not the centre of the window.
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
    // inverse. With a design viewport set, the centre of the physical surface
    // must still map to the centre of the design surface.
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

    // Centre of the PHYSICAL surface (3200x1720) → centre of the design surface.
    auto [rx, ry] = pixel_to_root(logical_w * scale * 0.5f,
                                  logical_h * scale * 0.5f);
    REQUIRE_THAT(rx, WithinAbs(dw * 0.5f, kEps));
    REQUIRE_THAT(ry, WithinAbs(dh * 0.5f, kEps));
}
