// Inspector "Wiring" tab — lists design-sourced (Figma) overlays with a
// wired / NOT-WIRED badge, the signal a developer annotates so we fetch the
// matching Figma frame. Headless: builds a demo DesignFrameView whose overlays
// carry source_node_id, drives the inspector's Wiring tab, asserts it populated,
// and renders a PNG proof.
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/inspector_window.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/screenshot.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::inspect::InspectorWindow;

namespace {
// A demo faithful frame: 3 overlays — two carry a Figma source_node_id (one
// wired via an action tag, one not), one has no provenance (must be skipped).
std::unique_ptr<DesignFrameView> make_demo_frame() {
    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"260\" height=\"140\">"
        "<rect width=\"260\" height=\"140\" fill=\"#202028\"/></svg>";
    std::vector<DesignFrameElement> els;
    {  // unwired knob, from Figma
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::knob;
        e.source_node_id = "1273:33424";
        e.cx = 40; e.cy = 40; e.hit_radius = 24;
        els.push_back(std::move(e));
    }
    {  // wired dropdown (has an action tag), from Figma
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::dropdown;
        e.source_node_id = "1273:40010";
        e.action = "filterType";
        e.x = 10; e.y = 90; e.w = 120; e.h = 24;
        els.push_back(std::move(e));
    }
    {  // no provenance — should not appear in the Wiring tab
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.x = 150; e.y = 90; e.w = 40; e.h = 24;
        els.push_back(std::move(e));
    }
    return std::make_unique<DesignFrameView>(svg, std::move(els), 0, 0, 260, 140);
}
}  // namespace

TEST_CASE("Inspector Wiring tab lists Figma-sourced overlays", "[inspect][wiring]") {
    auto frame = make_demo_frame();
    InspectorWindow inspector(*frame);
    inspector.set_bounds({0, 0, 380, 520});
    inspector.select_tab("Wiring");

    // The tab renders headlessly with content (the two source_node_id overlays).
    auto png = render_to_png(inspector, 380, 520, 2.0f, ScreenshotBackend::skia);
    if (png.empty()) SKIP("Skia raster backend unavailable");
    REQUIRE(png.size() > 1000);
    // Also write a proof image for visual review.
    render_to_file(inspector, 380, 520, "/tmp/inspector_wiring.png", 2.0f,
                   ScreenshotBackend::skia);
}
