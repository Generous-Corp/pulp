// Faithful Figma-vector specimen catalog components (DesignFrameView), generated
// by tools/import-design/make_catalog_component.py. Pins: each embedded SVG
// loads (non-empty panel), renders headlessly, and is registered in the catalog.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_system.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/range_slider_view.hpp>
#include <pulp/view/inline_value_editor_view.hpp>
#include <pulp/view/property_panel_view.hpp>
#include <pulp/view/group_box_view.hpp>
#include <pulp/view/number_box_states_view.hpp>
#include <pulp/view/knob_modulation_view.hpp>
#include <pulp/view/waveform_recorder_view.hpp>

using namespace pulp::view;

template <class T>
static void check_loads_and_renders() {
    T v;
    REQUIRE(v.panel_width() > 0.0f);
    REQUIRE(v.panel_height() > 0.0f);
    v.set_bounds({0.0f, 0.0f, 600.0f, 400.0f});
    auto png = render_to_png(v, 600, 400, 1.0f, ScreenshotBackend::skia);
    if (png.empty()) SKIP("Skia raster screenshot backend unavailable");  // no Skia (e.g. Windows CI)
    REQUIRE(png.size() > 1000);
}

TEST_CASE("Faithful specimens load + render", "[view][faithful]") {
    check_loads_and_renders<RangeSliderView>();
    check_loads_and_renders<InlineValueEditorView>();
    check_loads_and_renders<PropertyPanelView>();
    check_loads_and_renders<GroupBoxView>();
    check_loads_and_renders<NumberBoxStatesView>();
    check_loads_and_renders<KnobModulationView>();
    check_loads_and_renders<WaveformRecorderView>();
}

TEST_CASE("Faithful specimens are cataloged", "[design][catalog]") {
    for (const char* name : {"Range Slider", "Inline Value Editor", "Property Panel",
                             "Group Box", "Number Box", "Knob Modulation", "Waveform Recorder"}) {
        INFO(name);
        REQUIRE(pulp::design::find(name) != nullptr);
    }
}
