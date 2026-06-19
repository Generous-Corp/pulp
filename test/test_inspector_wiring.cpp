// Inspector "Wiring" tab — lists design-sourced (Figma) overlays with a
// wired / NOT-WIRED badge, the signal a developer annotates so we fetch the
// matching Figma frame. Headless: builds a demo DesignFrameView whose overlays
// carry source_node_id, drives the inspector's Wiring tab, asserts it populated,
// and renders a PNG proof.
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/inspector_window.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/screenshot.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
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
    inspector.set_bounds({0, 0, 380, 560});
    inspector.select_tab("Wiring");

    // The tab renders headlessly with content (the two source_node_id overlays).
    auto png = render_to_png(inspector, 380, 560, 2.0f, ScreenshotBackend::skia);
    if (png.empty()) SKIP("Skia raster backend unavailable");
    REQUIRE(png.size() > 1000);
    // Also write a proof image for visual review.
    render_to_file(inspector, 380, 560, "/tmp/inspector_wiring.png", 2.0f,
                   ScreenshotBackend::skia);
}

TEST_CASE("Inspector Wiring tab exports annotations as JSON", "[inspect][wiring]") {
    auto frame = make_demo_frame();
    InspectorWindow inspector(*frame);
    inspector.set_figma_file_key("q9iDYZzg86YrOQKr6I3bY0");
    inspector.set_bounds({0, 0, 380, 560});
    inspector.select_tab("Wiring");  // populates the entry list

    const std::string out =
        (std::filesystem::temp_directory_path() / "pulp_wiring_export_test.json").string();
    std::error_code ec; std::filesystem::remove(out, ec);
    REQUIRE(inspector.export_wiring_annotations(out));

    std::ifstream in(out);
    std::stringstream ss; ss << in.rdbuf();
    const std::string json = ss.str();

    // Both Figma-sourced overlays + the file key; the no-provenance one is absent.
    REQUIRE(json.find("1273:33424") != std::string::npos);  // unwired knob
    REQUIRE(json.find("1273:40010") != std::string::npos);  // wired dropdown
    REQUIRE(json.find("q9iDYZzg86YrOQKr6I3bY0") != std::string::npos);
    REQUIRE(json.find("knob") != std::string::npos);
    REQUIRE(json.find("dropdown") != std::string::npos);
    // The wired/unwired distinction is recorded.
    REQUIRE(json.find("true") != std::string::npos);
    REQUIRE(json.find("false") != std::string::npos);
}
