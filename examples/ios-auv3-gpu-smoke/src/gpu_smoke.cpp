#include "gpu_smoke.hpp"

#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

#include <cmath>
#include <memory>

namespace pulp::examples::ios_gpu_smoke {

using namespace pulp::format;
using namespace pulp::state;
using namespace pulp::audio;
using namespace pulp::midi;

namespace {

// View subclass with one animated 2D primitive driven off FrameClock.
// Flags `requires_gpu_host=true` so decide_gpu_host() routes through the
// Metal/Dawn PluginViewHost on iOS — that's the path Phase iOS-D.1 is
// proving end-to-end.
class GpuSmokeView : public pulp::view::View {
public:
    GpuSmokeView() {
        set_requires_gpu_host(true);
    }

    void on_attached() override {
        // Subscribe to the frame clock so we re-paint on every vsync tick
        // the iOS CADisplayLink delivers via PluginViewHost.
        if (auto* clock = frame_clock()) {
            sub_id_ = clock->subscribe([this](float dt) {
                t_ += dt;
                request_repaint();
                return true;
            });
        }
        pulp::runtime::log_info(
            "[gpu-smoke] view attached, awaiting FrameClock ticks");
    }

    void on_detached() override {
        if (auto* clock = frame_clock()) {
            if (sub_id_ != 0) clock->unsubscribe(sub_id_);
        }
        sub_id_ = 0;
    }

    void paint(pulp::canvas::Canvas& canvas) override {
        const auto r = local_bounds();
        // Background: dark blue so a CPU-fallback fill (pure black) is
        // immediately obvious if Skia/Dawn never came up.
        canvas.set_fill_color(pulp::canvas::Color::rgba(0.04f, 0.06f, 0.12f));
        canvas.fill_rect(r.x, r.y, r.width, r.height);

        // One rotating coloured quad in the centre. Cheap to draw, but
        // animates every frame so a Metal frame capture from Xcode shows
        // real command buffers landing on the iPad GPU.
        const float cx = r.x + r.width * 0.5f;
        const float cy = r.y + r.height * 0.5f;
        const float side = std::min(r.width, r.height) * 0.4f;

        const float angle = t_ * 1.2f; // ~0.2 turns/sec

        canvas.save();
        canvas.translate(cx, cy);
        canvas.rotate(angle);  // composes onto current transform
        const float hue = std::fmod(t_ * 60.0f, 360.0f);
        const auto col = pulp::canvas::Color::from_hsv(
            {hue, 0.8f, 1.0f});
        canvas.set_fill_color(col);
        canvas.fill_rect(-side * 0.5f, -side * 0.5f, side, side);
        canvas.restore();
    }

private:
    int sub_id_ = 0;
    float t_ = 0.0f;
};

} // namespace

PluginDescriptor GpuSmoke::descriptor() const {
    PluginDescriptor d;
    d.name = "Pulp GPU Smoke";
    d.manufacturer = "Pulp";
    d.bundle_id = "com.pulp.examples.gpusmoke";
    d.version = "0.1.0";
    d.category = PluginCategory::Effect;
    d.accepts_midi = false;
    d.produces_midi = false;
    d.input_buses = {{"Main In", 2, false}};
    d.output_buses = {{"Main Out", 2, false}};
    return d;
}

void GpuSmoke::define_parameters(StateStore& /*store*/) {
    // No parameters — keeps the AU surface as small as possible so the
    // proof is exclusively about the GPU view path.
}

void GpuSmoke::prepare(const PrepareContext& /*ctx*/) {}

void GpuSmoke::process(BufferView<float>& out,
                       const BufferView<const float>& in,
                       MidiBuffer& /*midi_in*/,
                       MidiBuffer& /*midi_out*/,
                       const ProcessContext& ctx) {
    // Bit-perfect bypass: copy input to output, fill silence on any
    // missing channel. Effect plug-ins with no audio path still must
    // not leave garbage in the output buffers.
    const int n = ctx.num_samples;
    const int out_chs = out.num_channels();
    const int in_chs = in.num_channels();
    for (int ch = 0; ch < out_chs; ++ch) {
        auto dst = out.channel(ch);
        if (ch < in_chs) {
            auto src = in.channel(ch);
            for (int i = 0; i < n; ++i) dst[i] = src[i];
        } else {
            for (int i = 0; i < n; ++i) dst[i] = 0.0f;
        }
    }
}

std::unique_ptr<pulp::view::View> GpuSmoke::create_view() {
    return std::make_unique<GpuSmokeView>();
}

} // namespace pulp::examples::ios_gpu_smoke
