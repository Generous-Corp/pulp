// The editors, rendered headless.
//
// A screenshot test that only asserts "the PNG is non-empty" passes on a blank
// canvas. So these assert the thing that actually matters for a CV editor: the
// readout *moves with the state*. Rendering the same view at two different
// parameter values must produce two different images. If the voltage rail is
// ever silently disconnected from the DSP, that assertion is what catches it —
// and on a plug-in that makes no sound, a dead readout is indistinguishable from
// a dead cable.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "cv_osc_processor.hpp"
#include "dc_processor.hpp"
#include "function_processor.hpp"
#include "lfo_processor.hpp"
#include "quantizer_processor.hpp"
#include "step_processor.hpp"
#include "sync_processor.hpp"

#include <pulp/format/headless.hpp>
#include <brew/ui/panel.hpp>
#include <pulp/view/screenshot.hpp>

#include <algorithm>
#include <string_view>
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

/// The toggle carrying `label`, anywhere in the tree, or null.
///
/// Searched by label rather than by child index: an editor's rows get reordered
/// whenever a control is added, and a test that pins an index would then assert
/// against whichever widget inherited the slot.
view::Toggle* find_toggle(view::View& root, std::string_view label) {
    if (auto* t = dynamic_cast<view::Toggle*>(&root); t && t->label() == label)
        return t;
    for (size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_toggle(*root.child_at(i), label)) return hit;
    return nullptr;
}

/// The bottom-right corner of the furthest-extending descendant, in the root's
/// coordinates. Yoga has already placed everything by the time this runs.
void content_extent(const view::View& v, float ox, float oy, float& w, float& h) {
    for (size_t i = 0; i < v.child_count(); ++i) {
        const view::View* c = v.child_at(i);
        const auto b = c->bounds();
        w = std::max(w, ox + b.x + b.width);
        h = std::max(h, oy + b.y + b.height);
        content_extent(*c, ox + b.x, oy + b.y, w, h);
    }
}

/// Push a constant level through both input channels for one block, so the
/// editors that display an operating point have a signal to point at.
void drive(format::HeadlessHost& host, float level) {
    constexpr std::size_t kFrames = 64;
    audio::Buffer<float> in(2, kFrames), out(2, kFrames);
    in.clear();
    out.clear();
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t n = 0; n < kFrames; ++n) in.channel(c)[n] = level;
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, kFrames);
    auto ov = out.view();
    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = kFrames;
    host.process(ov, iv, ctx);
}

}  // namespace

TEST_CASE("every editor fits inside the size it asks the host for",
          "[brew][ui][layout]") {
    // A DAW opens a plug-in at exactly `editor_size()`. Adding a control and
    // forgetting to grow the panel is a one-line mistake no screenshot test
    // notices, because a screenshot of a squashed editor still looks like an
    // editor.
    //
    // Measured against an unbounded height, NOT against `editor_size()` itself.
    // Yoga compresses a shrinkable child — the caption labels — to fit whatever
    // box it is given, so laying out at the declared size and asking "did the
    // content fit" always answers yes. It fits because Yoga made it fit. The
    // question worth asking is how tall the content wants to be.
    constexpr float kUnbounded = 4000.0f;

    struct Case { const char* name; format::ProcessorFactory factory; };
    const Case cases[] = {
        {"CV To OSC", create_cv_osc}, {"DC", create_dc},   {"Function", create_function},
        {"LFO", create_lfo},          {"Quantizer", create_quantizer},
        {"Step LFO", create_step},    {"Sync", create_sync},
    };

    for (const auto& c : cases) {
        Editor ed(c.factory);
        ed.view->set_bounds({0, 0, static_cast<float>(ed.width), kUnbounded});
        ed.view->layout_children();

        float w = 0.0f, h = 0.0f;
        content_extent(*ed.view, 0.0f, 0.0f, w, h);

        INFO(c.name << " asks the host for " << ed.width << "x" << ed.height
                    << " but its content wants " << w << "x" << h);
        CHECK(w <= static_cast<float>(ed.width));
        CHECK(h <= static_cast<float>(ed.height));
    }
}

TEST_CASE("DC's editor renders and tracks its output", "[brew][ui][dc]") {
    Editor ed(create_dc);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    // The rail shows the sample the DSP *emitted*, not the value the knobs ask
    // for — those differ whenever the input bus or the smoother is doing
    // anything. So every render here is preceded by a block.
    auto shoot_after_block = [&](float value) {
        ed.host.state().set_value(DcProcessor::kValue, value);
        drive(ed.host, 0.0f);
        return ed.shoot();
    };

    const auto at_zero = shoot_after_block(0.0f);
    REQUIRE_FALSE(at_zero.empty());

    // The rail must follow the DSP. Same view, different value, different pixels.
    const auto at_high = shoot_after_block(0.8f);
    REQUIRE_FALSE(at_high.empty());
    REQUIRE(differs(at_zero, at_high));

    // Polarity is visible too: +0.8 and -0.8 fill opposite halves of the rail.
    REQUIRE(differs(shoot_after_block(-0.8f), at_high));

    SECTION("scale attenuates what the rail shows") {
        const auto full = shoot_after_block(0.8f);
        ed.host.state().set_value(DcProcessor::kOutputScale, 0.1f);
        drive(ed.host, 0.0f);
        REQUIRE(differs(ed.shoot(), full));
    }

    SECTION("the rail follows the input bus, not just the knobs") {
        // Input Mul fully gates the output. Two different input levels must give
        // two different rails — a rail wired to the knobs could not tell them
        // apart, and that is precisely the dead-cable case it exists to catch.
        ed.host.state().set_value(DcProcessor::kValue, 1.0f);
        ed.host.state().set_value(DcProcessor::kInputMul, 1.0f);
        drive(ed.host, 0.25f);
        const auto quiet = ed.shoot();
        drive(ed.host, 0.9f);
        REQUIRE(differs(ed.shoot(), quiet));
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
    const auto sine = ed.shoot();   // sine at full depth is the default mix
    REQUIRE_FALSE(sine.empty());

    // The scope reads the plug-in's own value_at(), so folding another shape into
    // the mix must change the picture. A scope that renders a fixed curve is
    // decoration.
    ed.host.state().set_value(LfoProcessor::kSquare, 0.6f);
    REQUIRE(differs(ed.shoot(), sine));

    SECTION("and the phase changes it too") {
        ed.host.state().set_value(LfoProcessor::kSquare, 0.0f);
        const auto at_1 = ed.shoot();
        ed.host.state().set_value(LfoProcessor::kPhaseDegrees, 90.0f);
        REQUIRE(differs(ed.shoot(), at_1));
    }
}

TEST_CASE("LFO's Free Run toggle reaches the rate mode", "[brew][ui][lfo]") {
    Editor ed(create_lfo);

    // The scope is deliberately mode-agnostic — it sweeps one cycle either way —
    // so no pixel comparison can prove this switch is connected. Without this
    // test, deleting the toggle from the editor leaves the whole suite green and
    // strands a shipped parameter with no way to reach it.
    auto* toggle = find_toggle(*ed.view, "Free Run");
    REQUIRE(toggle != nullptr);
    REQUIRE_FALSE(toggle->is_on());
    REQUIRE(ed.host.state().get_value(LfoProcessor::kRateMode) == 0.0f);

    toggle->on_mouse_down(view::Point{});
    REQUIRE(toggle->is_on());
    REQUIRE(ed.host.state().get_value(LfoProcessor::kRateMode) == 1.0f);

    toggle->on_mouse_down(view::Point{});
    REQUIRE(ed.host.state().get_value(LfoProcessor::kRateMode) == 0.0f);

    // Same blind spot, same fix: swing warps the timeline, not the shape, so the
    // scope cannot see which unit is selected either.
    auto* sixteenths = find_toggle(*ed.view, "16ths");
    REQUIRE(sixteenths != nullptr);
    sixteenths->on_mouse_down(view::Point{});
    REQUIRE(ed.host.state().get_value(LfoProcessor::kSwingUnit) == 1.0f);
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

        const auto* fn = static_cast<const FunctionProcessor*>(ed.host.processor());

        drive(ed.host, -0.9f);
        REQUIRE(fn->display_input() == -0.9f);
        const auto low = ed.shoot();

        drive(ed.host, 0.9f);
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

TEST_CASE("Quantizer's editor draws the staircase and the operating point",
          "[brew][ui][quantizer]") {
    Editor ed(create_quantizer);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    const auto twelve = ed.shoot();
    REQUIRE_FALSE(twelve.empty());

    // The graph evaluates the plug-in's own transfer function, so changing the
    // step count must redraw it. A fixed staircase would be decoration.
    ed.host.state().set_value(QuantizerProcessor::kSteps, 4.0f);
    const auto four = ed.shoot();
    REQUIRE(differs(twelve, four));

    SECTION("offset slides the lattice") {
        ed.host.state().set_value(QuantizerProcessor::kOffset, 0.5f);
        REQUIRE(differs(ed.shoot(), four));
    }

    SECTION("the dot follows the incoming voltage") {
        // Two inputs that land on different treads must move the dot. This is the
        // only thing in the editor that distinguishes a live cable from a dead one.
        drive(ed.host, -0.7f);
        const auto low = ed.shoot();
        drive(ed.host, 0.7f);
        REQUIRE(differs(ed.shoot(), low));
    }
}

TEST_CASE("Step LFO's editor draws the pattern and marks the playing step",
          "[brew][ui][step]") {
    Editor ed(create_step);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    const auto ramp = ed.shoot();
    REQUIRE_FALSE(ramp.empty());

    // The bars read the store, so editing a step must redraw them.
    ed.host.state().set_value(StepProcessor::step_param(3), -0.9f);
    REQUIRE(differs(ed.shoot(), ramp));

    SECTION("shortening the pattern recesses the steps that no longer play") {
        const auto full = ed.shoot();
        ed.host.state().set_value(StepProcessor::kLength, 3.0f);
        REQUIRE(differs(ed.shoot(), full));
    }

    SECTION("clicking a bar writes the step under the cursor, and only it") {
        // The bars are the control, not a picture of one. Render once so flex has
        // placed them, then click inside the first bar near its top.
        (void)ed.shoot();
        const view::Rect bars = ed.view->child_at(0)->bounds();
        REQUIRE(bars.width > 0.0f);

        const float slot = bars.width / 8.0f;
        const float x = bars.x + slot * 0.5f;          // centre of bar 0
        const float y = bars.y + bars.height * 0.2f;   // 20% down => +0.6

        const float neighbour_before =
            ed.host.state().get_value(StepProcessor::step_param(1));
        ed.view->simulate_click({x, y});

        REQUIRE(ed.host.state().get_value(StepProcessor::step_param(0)) ==
                Catch::Approx(0.6f).margin(0.02f));
        // A click on one bar must not disturb its neighbour.
        REQUIRE(ed.host.state().get_value(StepProcessor::step_param(1)) ==
                neighbour_before);
    }

    SECTION("a drag keeps writing the bar it started on") {
        // A drag that wandered sideways must keep writing the step it began on.
        // `ParameterEdit` already refuses writes outside its gesture, so a
        // re-picking handler could not corrupt a neighbour — it would instead go
        // quietly dead the moment the cursor left the first bar. So the assertion
        // that matters is the *final* value, not merely that something changed.
        (void)ed.shoot();
        const view::Rect bars = ed.view->child_at(0)->bounds();
        const float slot = bars.width / 8.0f;
        const float mid_y = bars.y + bars.height * 0.5f;

        std::vector<float> before;
        for (int i = 0; i < 8; ++i)
            before.push_back(ed.host.state().get_value(StepProcessor::step_param(i)));

        // Start in bar 0 at the zero line, drag right across bars 1..3 and up to
        // 10% from the top, which is +0.8.
        ed.view->simulate_drag({bars.x + slot * 0.5f, mid_y},
                               {bars.x + slot * 3.5f, bars.y + bars.height * 0.1f});

        REQUIRE(ed.host.state().get_value(StepProcessor::step_param(0)) ==
                Catch::Approx(0.8f).margin(0.02f));
        for (int i = 1; i < 8; ++i) {
            CAPTURE(i);
            REQUIRE(ed.host.state().get_value(StepProcessor::step_param(i)) ==
                    before[static_cast<std::size_t>(i)]);
        }
    }
}

TEST_CASE("CV To OSC's editor tracks the observed voltage", "[brew][ui][osc]") {
    Editor ed(create_cv_osc);
    if (!ed.can_capture()) {
        WARN("no raster screenshot backend in this build — skipping");
        return;
    }
    // Send is off, and rendering an editor must never turn it on.
    REQUIRE(ed.host.state().get_value(CvOscProcessor::kEnabled) == 0.0f);

    drive(ed.host, 0.2f);
    const auto low = ed.shoot();
    REQUIRE_FALSE(low.empty());

    // The rail reads the input the DSP saw, so a different voltage redraws it.
    drive(ed.host, -0.8f);
    REQUIRE(differs(ed.shoot(), low));

    const auto* proc = static_cast<const CvOscProcessor*>(ed.host.processor());
    REQUIRE(proc->latest(0) == -0.8f);
    // Nothing was sent, because nothing was asked for.
    REQUIRE(proc->sent_count() == 0);
}
