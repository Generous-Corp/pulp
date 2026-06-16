// ChannelStripView — faithful Figma-vector catalog component (DesignFrameView).
// Pins: embedded SVG loads, headless render, catalog registration.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_system.hpp>
#include <pulp/view/channel_strip_view.hpp>
#include <pulp/view/screenshot.hpp>

using namespace pulp::view;

TEST_CASE("ChannelStripView loads its embedded faithful SVG", "[view][channel-strip]") {
    ChannelStripView v;
    REQUIRE(v.panel_width() > 0.0f);
    REQUIRE(v.panel_height() > 0.0f);
}

TEST_CASE("ChannelStripView renders headlessly", "[view][channel-strip]") {
    ChannelStripView v;
    v.set_bounds({0.0f, 0.0f, 900.0f, 300.0f});
    auto png = render_to_png(v, 900, 300, 1.0f, ScreenshotBackend::skia);
    if (png.empty()) SKIP("Skia raster screenshot backend unavailable");  // no Skia (e.g. Windows CI)
    REQUIRE(png.size() > 1000);
}

TEST_CASE("Channel Strip is registered in the pulp::design catalog", "[design][catalog]") {
    const auto* info = pulp::design::find("Channel Strip");
    REQUIRE(info != nullptr);
    REQUIRE(info->native_class == "pulp::view::ChannelStripView");
    REQUIRE(info->category == pulp::design::Category::containers);
}
