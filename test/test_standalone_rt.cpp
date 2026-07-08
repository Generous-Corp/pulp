// Standalone host render-path RT-safety guard.
//
// Task 1.3 (planning/2026-07-08-remaining-open-items-implementation-plan.md
// §1.3): StandaloneApp wraps `Processor::process()` in a
// `pulp::runtime::ScopedNoAlloc` inside the audio device callback
// (standalone.cpp). The callback body was extracted into the private
// `StandaloneApp::render_audio_block()` seam (the device lambda is now a thin
// wrapper) so it can be driven without opening real audio hardware. This test
// prepares a StandaloneApp headlessly and drives one steady-state block,
// asserting the whole callback body neither allocates nor takes a blocking lock.
//
// Mechanism: built with PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1 and linked
// against the trap TU (test/native_components/rt_intercept_test_support.cpp),
// `ScopedRtProcessProbe` enters an always-on RtNoAllocScope whose strong
// operator-new / pthread-lock overrides ABORT on an allocation or blocking lock
// in scope. See test/harness/scoped_rt_process_probe.hpp.
//
// The first two render calls are warm-up: the meter bridges / output probe do
// their one-time steady-state priming outside the probe scope (as they would on
// a host's first callbacks). The measured call is the steady-state block, where
// the callback body must be allocation/lock-free.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/device.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>

#include "harness/scoped_rt_process_probe.hpp"

#include <array>
#include <cstddef>
#include <memory>

using namespace pulp;

namespace {

// Minimal RT-safe stereo effect: copies input to output. Real per-sample work
// that touches no heap and takes no lock.
class RtEffectProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "StandaloneRtEffect",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.standalone-rt",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "",
            .range = {0.0f, 1.0f, 1.0f, 0.01f},
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float gain = state().get_value(1);
        const std::size_t channels = output.num_channels();
        const std::size_t samples = output.num_samples();
        for (std::size_t c = 0; c < channels; ++c) {
            float* dst = output.channel_ptr(c);
            const bool has_in = c < input.num_channels();
            const float* src = has_in ? input.channel_ptr(c) : nullptr;
            for (std::size_t n = 0; n < samples; ++n) {
                dst[n] = has_in ? src[n] * gain : 0.0f;
            }
        }
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& context) override {
        auto* out = audio.main_output();
        auto* in = audio.main_input();
        audio::BufferView<const float> empty_input;
        if (out) {
            process(*out, in ? *in : empty_input, midi_in, midi_out, context);
        }
    }
};

std::unique_ptr<format::Processor> create_rt_effect() {
    return std::make_unique<RtEffectProcessor>();
}

}  // namespace

// Friend accessor: reaches StandaloneApp's private render seam so the test can
// prepare + drive it without start()ing a real audio device. Declared a friend
// in standalone.hpp (mirrors the @internal hook precedent).
namespace pulp::format {
struct StandaloneRenderTestAccess {
    static void ensure_processor(StandaloneApp& app) {
        if (!app.processor_) {
            app.processor_ = app.factory_();
            app.processor_->set_state_store(&app.store_);
            app.processor_->define_parameters(app.store_);
        }
    }
    static void prepare(StandaloneApp& app) { app.prepare_render_state(); }
    static void render(StandaloneApp& app,
                       const audio::BufferView<const float>& input,
                       audio::BufferView<float>& output,
                       const audio::CallbackContext& ctx) {
        app.render_audio_block(input, output, ctx);
    }
};
}  // namespace pulp::format

TEST_CASE("Standalone render_audio_block is allocation/lock-free",
          "[standalone][format][rt-safety]") {
    using pulp::format::StandaloneRenderTestAccess;

    constexpr int kFrames = 256;

    format::StandaloneConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = kFrames;
    cfg.output_channels = 2;
    cfg.input_channels = 2;
    cfg.persist_settings = false;       // no ApplicationProperties I/O in a test
    cfg.transport_playing = false;      // skip the transport-clock atomic advance
    cfg.route_test_signal_to_output = false;

    format::StandaloneApp app(create_rt_effect);
    app.set_config(cfg);
    StandaloneRenderTestAccess::ensure_processor(app);
    StandaloneRenderTestAccess::prepare(app);

    // Test-owned input / output storage; deinterleaved stereo.
    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    std::array<float, kFrames> out_l{};
    std::array<float, kFrames> out_r{};
    for (int n = 0; n < kFrames; ++n) {
        in_l[static_cast<std::size_t>(n)] = 0.1f;
        in_r[static_cast<std::size_t>(n)] = -0.1f;
    }
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    audio::BufferView<const float> input(in_ptrs, 2, kFrames);
    audio::BufferView<float> output(out_ptrs, 2, kFrames);

    audio::CallbackContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.buffer_size = kFrames;
    ctx.sample_position = 0;

    // Warm-up: prime the meter bridges / output probe steady state.
    StandaloneRenderTestAccess::render(app, input, output, ctx);
    StandaloneRenderTestAccess::render(app, input, output, ctx);

    std::size_t allocation_count = 0;
    {
        pulp::test::ScopedRtProcessProbe probe;
        StandaloneRenderTestAccess::render(app, input, output, ctx);
        allocation_count = probe.allocation_count();
    }
    REQUIRE(allocation_count == 0);

    // Sanity: the effect actually produced output (gain default 1.0).
    REQUIRE(out_l[0] == 0.1f);
    REQUIRE(out_r[0] == -0.1f);
}
