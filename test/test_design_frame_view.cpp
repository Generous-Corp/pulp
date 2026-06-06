// DesignFrameView — the faithful-vector design-import view (Plan B / B1).
//
// Renders a design's own SVG via Canvas::draw_svg (SkSVGDOM), cropped to its
// panel, and overlays interactive knobs from a TYPED element list (the importer
// supplies it — the view does not guess from the SVG). These tests pin: panel
// auto-detect, knob hit-test + value drag, the needle visibly rotating in the
// rendered output, and fail-safe behavior — with no design-import dependency.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <string>

using namespace pulp::view;

namespace {

// A minimal "design": an 80x80 panel rect at (10,10), one knob (a gradient-less
// dome circle at (50,50) r20) with a vertical needle pointing up.
std::string make_design_svg() {
    return R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
           R"SVG(<rect x="10" y="10" width="80" height="80" rx="4" fill="#cccccc"/>)SVG"
           R"SVG(<circle cx="50" cy="50" r="20" fill="#8a97a6"/>)SVG"
           R"SVG(<path d="M50 38L50 30" stroke="white" stroke-width="3"/></svg>)SVG";
}

DesignFrameElement make_knob() {
    DesignFrameElement k;
    k.kind = DesignFrameElement::Kind::knob;
    k.cx = 50; k.cy = 50; k.hit_radius = 22;
    k.needle_d = "M50 38L50 30";
    k.value = 0.5f;
    return k;
}

}  // namespace

TEST_CASE("DesignFrameView auto-detects the panel and holds typed elements",
          "[view][design-import][frame][svg]") {
    DesignFrameView v(make_design_svg(), {make_knob()});
    REQUIRE(v.element_count() == 1);
    CHECK(v.panel_width() == 80.0f);   // the 80x80 panel rect, not the 100x100 frame
    CHECK(v.panel_height() == 80.0f);
    CHECK(v.element_value(0) == 0.5f);
    CHECK(v.element_value(99) == -1.0f);
}

TEST_CASE("DesignFrameView drag turns the hit knob", "[view][design-import][frame]") {
    DesignFrameView v(make_design_svg(), {make_knob()});
    v.set_bounds({0, 0, 80, 80});  // view == panel, so view coords map 1:1

    // Click the dome center, drag UP — value should rise.
    v.on_mouse_down({40, 40});     // -> SVG (50,50), inside the knob
    v.on_mouse_drag({40, 10});     // drag up 30px
    CHECK(v.element_value(0) > 0.6f);

    // A click far from any knob hits nothing — value unchanged.
    v.set_element_value(0, 0.5f);
    v.on_mouse_down({2, 2});
    v.on_mouse_drag({2, -40});
    CHECK(v.element_value(0) == 0.5f);
}

TEST_CASE("DesignFrameView renders faithfully and the needle visibly rotates",
          "[view][design-import][frame][svg]") {
    DesignFrameView lo(make_design_svg(), {make_knob()});
    DesignFrameView hi(make_design_svg(), {make_knob()});
    lo.set_bounds({0, 0, 80, 80});
    hi.set_bounds({0, 0, 80, 80});
    lo.set_element_value(0, 0.1f);
    hi.set_element_value(0, 0.9f);

    auto lo_png = render_to_png(lo, 80, 80, 2.0f, ScreenshotBackend::skia);
    if (lo_png.empty()) SKIP("Skia raster screenshot backend unavailable");
    auto hi_png = render_to_png(hi, 80, 80, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(hi_png.empty());

    // The chrome is identical; only the needle rotated — so the two renders must
    // differ (proves draw_svg drew the design AND the needle moved with value).
    const auto cmp = compare_screenshots(lo_png, hi_png);
    REQUIRE(cmp.valid);
    CHECK(cmp.similarity < 0.999f);
}

TEST_CASE("DesignFrameView is fail-safe on an empty/garbage SVG",
          "[view][design-import][frame][svg]") {
    DesignFrameView empty("", {});
    empty.set_bounds({0, 0, 40, 40});
    CHECK(empty.element_count() == 0);
    // Painting must not crash even with no SVG (the panel falls back to 0 -> early
    // out). Render path returns an empty/uniform image; the call must be safe.
    auto png = render_to_png(empty, 40, 40, 1.0f, ScreenshotBackend::skia);
    SUCCEED("paint with empty SVG did not crash");
}
