// The editors, rendered headless.
//
// A screenshot test that only asserts "the PNG is non-empty" passes on a blank
// canvas. So these assert the thing that actually matters for a CV editor: the
// readout *moves with the state*. Rendering the same view at two different
// parameter values must produce two different images. If the voltage rail is
// ever silently disconnected from the DSP, that assertion is what catches it —
// and on a plug-in that makes no sound, a dead readout is indistinguishable from
// a dead cable.

#include <catch2/catch_test_macros.hpp>

#include "dc_processor.hpp"
#include "function_processor.hpp"
#include "lfo_processor.hpp"
#include "sync_processor.hpp"

#include <pulp/format/headless.hpp>
#include <brew/ui/panel.hpp>
#include <pulp/view/screenshot.hpp>

#include <tuple>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::examples::brew;

namespace {

/// Build a plug-in's editor through the real Processor path, at the size the
/// plug-in tells a host to open it at — so the test cannot drift from what a DAW
/// would actually construct, nor pass at a geometry no host ever uses.
struct Editor {
    explicit Editor(format::ProcessorFactory factory) : host(std::move(factory)) {
        host.prepare(48000.0, 512, 2, 2);
        std::tie(width, height) = host.processor()->editor_size();
        REQUIRE(width > 0);
        REQUIRE(height > 0);
        view = host.processor()->create_view();
        REQUIRE(view != nullptr);
        view->set_bounds({0, 0, static_cast<float>(width),
                          static_cast<float>(height)});
    }

    std::vector<uint8_t> shoot() {
        return view::render_to_png(*view, width, height, 2.0f);
    }

    /// True when this build has a raster backend at all. `has_screenshot_provider()`
    /// is NOT that question — it reports whether a *host-registered* provider
    /// exists, and says nothing about macOS's native CoreGraphics path. Gating on
    /// it turned a runnable test into a silent skip.
    bool can_capture() {
        const bool empty = shoot().empty();
#if defined(__APPLE__)
        // macOS always has the native backend. An empty render here is a real
        // failure, not an unsupported platform.
        REQUIRE_FALSE(empty);
#endif
        return !empty;
    }

    format::HeadlessHost host;
    std::unique_ptr<view::View> view;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// Compare renders without letting Catch2 stringify two 40 KB byte vectors on
/// failure — it produces megabytes of output and takes the process down with it.
bool differs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    return a != b;
}

}  // namespace

TEST_CASE("DC's editor renders and tracks its output", "[brew][ui][dc]") {
    Editor ed(create_dc);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    ed.host.state().set_value(DcProcessor::kValue, 0.0f);
    const auto at_zero = ed.shoot();
    REQUIRE_FALSE(at_zero.empty());

    // The rail must follow the DSP. Same view, different value, different pixels.
    ed.host.state().set_value(DcProcessor::kValue, 0.8f);
    const auto at_high = ed.shoot();
    REQUIRE_FALSE(at_high.empty());
    REQUIRE(differs(at_zero, at_high));

    // Polarity is visible too: +0.8 and -0.8 fill opposite halves of the rail.
    ed.host.state().set_value(DcProcessor::kValue, -0.8f);
    REQUIRE(differs(ed.shoot(), at_high));

    SECTION("scale attenuates what the rail shows") {
        ed.host.state().set_value(DcProcessor::kValue, 0.8f);
        const auto full = ed.shoot();
        ed.host.state().set_value(DcProcessor::kOutputScale, 0.1f);
        REQUIRE(differs(ed.shoot(), full));
    }
}

TEST_CASE("Sync's editor renders and its lamps follow the transport",
          "[brew][ui][sync]") {
    Editor ed(create_sync);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    const auto stopped = ed.shoot();
    REQUIRE_FALSE(stopped.empty());

    // Drive one block of transport through the processor. The run lamp lights,
    // so the image must change — the lamps read real DSP state, not a mock.
    audio::Buffer<float> in(2, 512), out(2, 512);
    in.clear();
    out.clear();
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, 512);
    auto ov = out.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 512;
    ctx.is_playing = true;
    ctx.transport_started = true;
    ctx.tempo_bpm = 120.0;
    ctx.position_beats = 0.0;
    ed.host.process(ov, iv, ctx);

    // Assert the published state first: if the lamps are dark, the pixel
    // comparison below would fail for a reason the diff cannot explain.
    const auto* sync = static_cast<const SyncProcessor*>(ed.host.processor());
    REQUIRE(sync->run_lamp() == 1.0f);
    REQUIRE(sync->clock_lamp() == 1.0f);

    REQUIRE(differs(ed.shoot(), stopped));
}

namespace {
struct RectChild : view::View {
    float g = 0.0f;
    void paint(canvas::Canvas& c) override {
        c.set_fill_color(canvas::Color::rgba8(255, static_cast<uint8_t>(g * 255), 0));
        c.fill_rect(0, 0, local_bounds().width, local_bounds().height);
    }
};
}  // namespace

TEST_CASE("LFO's editor draws the selected shape", "[brew][ui][lfo]") {
    Editor ed(create_lfo);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    ed.host.state().set_value(LfoProcessor::kWaveform,
                              static_cast<float>(Waveform::sine));
    const auto sine = ed.shoot();
    REQUIRE_FALSE(sine.empty());

    // The scope reads the plug-in's own value_at(), so changing the shape must
    // change the picture. A scope that renders a fixed curve is decoration.
    ed.host.state().set_value(LfoProcessor::kWaveform,
                              static_cast<float>(Waveform::square));
    REQUIRE(differs(ed.shoot(), sine));

    SECTION("and the rate changes it too") {
        ed.host.state().set_value(LfoProcessor::kWaveform,
                                  static_cast<float>(Waveform::sine));
        const auto at_1 = ed.shoot();
        ed.host.state().set_value(LfoProcessor::kPhaseDegrees, 90.0f);
        REQUIRE(differs(ed.shoot(), at_1));
    }
}

TEST_CASE("Function's editor draws the curve and tracks the signal on it",
          "[brew][ui][function]") {
    Editor ed(create_function);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    const auto linear = ed.shoot();
    REQUIRE_FALSE(linear.empty());

    // The graph evaluates the plug-in's own function_transfer(), so bending the
    // curve must bend the picture.
    ed.host.state().set_value(FunctionProcessor::kCurve,
                              static_cast<float>(Curve::exponential));
    ed.host.state().set_value(FunctionProcessor::kAmount, 4.0f);
    REQUIRE(differs(ed.shoot(), linear));

    SECTION("and the dot follows the incoming voltage") {
        // Back to linear so the *curve* is identical between the two renders and
        // the only thing that can move is the operating point.
        ed.host.state().set_value(FunctionProcessor::kCurve,
                                  static_cast<float>(Curve::linear));

        auto drive = [&](float level) {
            audio::Buffer<float> in(2, 64), out(2, 64);
            in.clear();
            out.clear();
            for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t n = 0; n < 64; ++n) in.channel(c)[n] = level;
            const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
            audio::BufferView<const float> iv(ip, 2, 64);
            auto ov = out.view();
            format::ProcessContext ctx;
            ctx.sample_rate = 48000.0;
            ctx.num_samples = 64;
            ed.host.process(ov, iv, ctx);
        };

        const auto* fn = static_cast<const FunctionProcessor*>(ed.host.processor());

        drive(-0.9f);
        REQUIRE(fn->display_input() == -0.9f);
        const auto low = ed.shoot();

        drive(0.9f);
        REQUIRE(fn->display_input() == 0.9f);
        REQUIRE(differs(ed.shoot(), low));
    }
}

// Yoga owns child geometry. `render_to_png` (and the window host) call
// `layout_children()` after `set_bounds()`, so bounds a view assigns by hand are
// overwritten before it paints — a child laid out that way renders at zero size
// and is silently invisible. This guards the flex-only rule.
TEST_CASE("a child laid out by flex paints into the raster", "[brew][ui][probe]") {
    ui::BrewPanel root("probe", "");
    auto child = std::make_unique<RectChild>();
    auto* cp = child.get();
    ui::fixed_size(*child, 60.0f, 40.0f);
    root.add_child(std::move(child));

    const auto a = view::render_to_png(root, 200, 200, 1.0f);
    REQUIRE_FALSE(a.empty());
    cp->g = 1.0f;
    REQUIRE(differs(a, view::render_to_png(root, 200, 200, 1.0f)));
}

// Isolation probe: does a Lamp on its own change pixels with its brightness?
TEST_CASE("Lamp brightness reaches the raster", "[brew][ui][probe]") {
    float b = 0.0f;
    ui::BrewPanel root("probe", "");
    auto lamp = std::make_unique<ui::Lamp>("X", [&b] { return b; });
    ui::fixed_size(*lamp, 48.0f, 44.0f);
    root.add_child(std::move(lamp));

    const auto dark = view::render_to_png(root, 200, 200, 1.0f);
    REQUIRE_FALSE(dark.empty());
    b = 1.0f;
    REQUIRE(differs(dark, view::render_to_png(root, 200, 200, 1.0f)));
}
