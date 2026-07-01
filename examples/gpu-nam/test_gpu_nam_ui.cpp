// GPU NAM editor tests: the faithful editor renders a non-blank, asset-composited
// frame through the Skia backend, and pointer input drives real parameters (knob
// drag, Noise-Gate / EQ toggles). Headless — no window, no audio device.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>

#include "gpu_nam_processor.hpp"
#include "gpu_nam_ui.hpp"

#include <memory>

using namespace pulp;
using namespace pulp::examples;

namespace {

std::unique_ptr<GpuNamProcessor> make_prepared(state::StateStore& store) {
    auto proc = std::make_unique<GpuNamProcessor>();
    proc->set_state_store(&store);
    proc->define_parameters(store);
    format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = 512;
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc->prepare(ctx);
    return proc;
}

GpuNamUi& as_ui(view::View& v) { return dynamic_cast<GpuNamUi&>(v); }

}  // namespace

TEST_CASE("GPU NAM editor renders a non-blank, asset-composited frame", "[nam][ui]") {
    state::StateStore store;
    auto proc = make_prepared(store);
    auto v = proc->create_view();
    REQUIRE(v);

    // Skia backend composites the reused bitmap/SVG chrome (CoreGraphics can't).
    auto res = view::capture_view(*v, 600, 400, 2.0f, view::ScreenshotBackend::skia);
    INFO("capture reason: " << res.reason);
    REQUIRE(res.ok);                 // passes the content floor — not a blank fill
    REQUIRE(res.png.size() > 5000);  // textured, composited chrome (not a flat rect)
    proc->release();
}

TEST_CASE("GPU NAM editor toggles drive Noise-Gate / EQ parameters", "[nam][ui]") {
    state::StateStore store;
    auto proc = make_prepared(store);
    auto v = proc->create_view();
    auto& ui = as_ui(*v);
    ui.set_bounds({0.0f, 0.0f, 600.0f, 400.0f});

    auto click = [&](view::Point p) { ui.on_mouse_down(p); ui.on_mouse_up(p); };

    const bool ng0 = store.get_value(kNoiseGateActive) >= 0.5f;
    click(ui.toggle_center_for_test(0));
    REQUIRE((store.get_value(kNoiseGateActive) >= 0.5f) != ng0);

    const bool eq0 = store.get_value(kEQActive) >= 0.5f;
    click(ui.toggle_center_for_test(1));
    REQUIRE((store.get_value(kEQActive) >= 0.5f) != eq0);
    proc->release();
}

TEST_CASE("GPU NAM editor knob drag changes the parameter", "[nam][ui]") {
    state::StateStore store;
    auto proc = make_prepared(store);
    auto v = proc->create_view();
    auto& ui = as_ui(*v);
    ui.set_bounds({0.0f, 0.0f, 600.0f, 400.0f});

    // Drag the Input knob (index 0) upward → value increases (up = louder).
    const view::Point c = ui.knob_center_for_test(0);
    const float v0 = store.get_value(kInputGain);
    ui.on_mouse_down(c);
    ui.on_mouse_drag({c.x, c.y - 120.0f});
    ui.on_mouse_up({c.x, c.y - 120.0f});
    REQUIRE(store.get_value(kInputGain) > v0);
    proc->release();
}
