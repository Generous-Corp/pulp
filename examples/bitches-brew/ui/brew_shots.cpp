#include "dc_processor.hpp"
#include "function_processor.hpp"
#include "lfo_processor.hpp"
#include "quantizer_processor.hpp"
#include "step_processor.hpp"
#include "sync_processor.hpp"
#include <pulp/format/headless.hpp>
#include <pulp/view/screenshot.hpp>
#include <cstdio>
using namespace pulp;
using namespace pulp::examples::brew;

/// Run one block so the editors that display real DSP state (Sync's lamps, LFO's
/// phase marker, Function's operating point) have something to show. `fill` seeds
/// the input bus for the plug-ins that read it.
static void run_block(format::HeadlessHost& h,
                      const format::ProcessContext& base,
                      float input = 0.0f) {
    constexpr std::size_t kFrames = 512;
    audio::Buffer<float> in(2, kFrames), out(2, kFrames);
    in.clear();
    out.clear();
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t n = 0; n < kFrames; ++n) in.channel(c)[n] = input;
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, kFrames);
    auto ov = out.view();
    format::ProcessContext c = base;
    c.sample_rate = 48000;
    c.num_samples = kFrames;
    h.process(ov, iv, c);
}

static format::ProcessContext playing(double beats) {
    format::ProcessContext c;
    c.is_playing = true;
    c.tempo_bpm = 120;
    c.position_beats = beats;
    return c;
}

static void shoot(format::ProcessorFactory f, const char* path,
                  void (*setup)(format::HeadlessHost&)) {
    format::HeadlessHost host(f);
    host.prepare(48000.0, 512, 2, 2);
    if (setup) setup(host);
    // Render at the size the plug-in tells a host to open it at, so a
    // screenshot cannot flatter a layout that a DAW would never show.
    const auto [w, h] = host.processor()->editor_size();
    auto v = host.processor()->create_view();
    v->set_bounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
    const bool ok = view::render_to_file(*v, w, h, path, 2.0f);
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", path);
}

int main() {
    shoot(create_dc, "/tmp/brewshots/dc.png", [](format::HeadlessHost& h) {
        h.state().set_value(DcProcessor::kValue, 0.62f);
    });
    shoot(create_dc, "/tmp/brewshots/dc-negative.png", [](format::HeadlessHost& h) {
        h.state().set_value(DcProcessor::kValue, -0.45f);
    });
    shoot(create_lfo, "/tmp/brewshots/lfo.png", [](format::HeadlessHost& h) {
        // A mix, not a shape: mostly sine with a triangle folded in and a little
        // asymmetry, which is the thing a selector could never do.
        h.state().set_value(LfoProcessor::kTriangle, 0.35f);
        h.state().set_value(LfoProcessor::kAsymmetry, 0.35f);
        run_block(h, playing(0.3));
    });
    shoot(create_function, "/tmp/brewshots/function.png",
          [](format::HeadlessHost& h) {
              h.state().set_value(FunctionProcessor::kCurve,
                                  static_cast<float>(Curve::exponential));
              h.state().set_value(FunctionProcessor::kAmount, 3.0f);
              run_block(h, playing(0.0), 0.7f);
          });
    shoot(create_quantizer, "/tmp/brewshots/quantizer.png",
          [](format::HeadlessHost& h) {
              // Few enough steps that the treads are legible, offset off the
              // lattice, and an input parked between two of them.
              h.state().set_value(QuantizerProcessor::kSteps, 6.0f);
              h.state().set_value(QuantizerProcessor::kOffset, 0.3f);
              run_block(h, playing(0.0), 0.42f);
          });
    shoot(create_step, "/tmp/brewshots/step.png", [](format::HeadlessHost& h) {
        const float shape[8] = {-0.2f, 0.55f, -0.75f, 0.35f,
                                0.9f,  -0.4f, 0.15f,  -0.9f};
        for (int i = 0; i < 8; ++i)
            h.state().set_value(StepProcessor::step_param(i), shape[i]);
        h.state().set_value(StepProcessor::kLength, 6.0f);
        h.state().set_value(StepProcessor::kSpeedMode, 1.0f);
        h.state().set_value(StepProcessor::kRate, 1.0f);
        run_block(h, playing(2.3));  // lights step 2
    });
    shoot(create_sync, "/tmp/brewshots/sync-stopped.png", nullptr);
    shoot(create_sync, "/tmp/brewshots/sync-running.png",
          [](format::HeadlessHost& h) {
              auto c = playing(0.0);
              c.transport_started = true;
              run_block(h, c);
          });
    return 0;
}
