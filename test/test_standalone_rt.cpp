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

#include <algorithm>
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

class InspectorSynthFixture final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "InspectorSynthFixture",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.inspector-synth-fixture",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Output", 1}},
            .accepts_midi = true,
        };
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override { active_ = false; }
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        std::size_t event_index = 0;
        for (std::size_t sample = 0; sample < output.num_samples(); ++sample) {
            while (event_index < midi_in.size() &&
                   midi_in[event_index].sample_offset <= static_cast<int>(sample)) {
                const auto& event = midi_in[event_index++];
                if (event.note() == 64 && event.is_note_on()) active_ = true;
                if (event.note() == 64 && event.is_note_off()) active_ = false;
            }
            for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
                output.channel_ptr(channel)[sample] = active_ ? 0.25f : 0.0f;
        }
    }
private:
    bool active_ = false;
};

class InspectorEffectFixture final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "InspectorEffectFixture",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.inspector-effect-fixture",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Input", 1}},
            .output_buses = {{"Output", 1}},
            .accepts_midi = true,
        };
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        saw_midi = !midi_in.empty();
        for (std::size_t sample = 0; sample < output.num_samples(); ++sample)
            output.channel_ptr(0)[sample] = input.channel_ptr(0)[sample] * 2.0f;
    }
    bool saw_midi = false;
};

InspectorEffectFixture* effect_fixture = nullptr;

std::unique_ptr<format::Processor> create_inspector_synth_fixture() {
    return std::make_unique<InspectorSynthFixture>();
}

std::unique_ptr<format::Processor> create_inspector_effect_fixture() {
    auto result = std::make_unique<InspectorEffectFixture>();
    effect_fixture = result.get();
    return result;
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

TEST_CASE("Standalone inspector synth note is audible and release-bounded",
          "[standalone][inspect][test-input][synth]") {
    using pulp::format::StandaloneRenderTestAccess;
    constexpr int kFrames = 128;
    constexpr float kVoiceLevel = 0.25f;

    format::StandaloneConfig cfg;
    cfg.sample_rate = 48'000.0;
    cfg.buffer_size = kFrames;
    cfg.input_channels = 0;
    cfg.output_channels = 1;
    cfg.transport_playing = false;
    cfg.persist_settings = false;

    format::StandaloneApp app(create_inspector_synth_fixture);
    app.set_config(cfg);
    StandaloneRenderTestAccess::ensure_processor(app);
    StandaloneRenderTestAccess::prepare(app);
    REQUIRE(app.test_input_host().inject_note({
                .kind = format::detail::StandaloneTestMidiKind::NoteOn,
                .channel = 1,
                .note = 64,
                .velocity = 100,
            }) == format::detail::StandaloneTestInputResult::Applied);
    app.test_input_host().release_test_input();

    std::array<float, kFrames> rendered{};
    float* output_ptrs[] = {rendered.data()};
    audio::BufferView<const float> input;
    audio::BufferView<float> output(output_ptrs, 1, kFrames);
    audio::CallbackContext context{.sample_rate = 48'000.0,
                                   .buffer_size = kFrames};
    StandaloneRenderTestAccess::render(app, input, output, context);

    const auto peak = *std::max_element(rendered.begin(), rendered.end());
    REQUIRE(peak == kVoiceLevel);
    REQUIRE(rendered.front() == kVoiceLevel);
    REQUIRE(rendered.back() == 0.0f);

    rendered.fill(1.0f);
    StandaloneRenderTestAccess::render(app, input, output, context);
    REQUIRE(std::all_of(rendered.begin(), rendered.end(),
                        [](float sample) { return sample == 0.0f; }));
}

TEST_CASE("Standalone inspector effect transforms test audio and direct routing still processes MIDI",
          "[standalone][inspect][test-input][effect][routing]") {
    using pulp::format::StandaloneRenderTestAccess;
    constexpr int kFrames = 256;
    constexpr float kInputPeak = 0.1f;
    constexpr float kEffectPeak = kInputPeak * 2.0f;

    format::StandaloneConfig cfg;
    cfg.sample_rate = 48'000.0;
    cfg.buffer_size = kFrames;
    cfg.input_channels = 1;
    cfg.output_channels = 1;
    cfg.transport_playing = false;
    cfg.persist_settings = false;
    cfg.route_test_signal_to_output = false;

    effect_fixture = nullptr;
    format::StandaloneApp app(create_inspector_effect_fixture);
    app.set_config(cfg);
    StandaloneRenderTestAccess::ensure_processor(app);
    REQUIRE(effect_fixture != nullptr);
    app.test_signal().set_config({.type = format::TestSignalType::sine,
                                  .sine_frequency_hz = 750.0f,
                                  .sine_amplitude = kInputPeak});
    StandaloneRenderTestAccess::prepare(app);

    std::array<float, kFrames> input_storage{};
    std::array<float, kFrames> rendered{};
    const float* input_ptrs[] = {input_storage.data()};
    float* output_ptrs[] = {rendered.data()};
    audio::BufferView<const float> input(input_ptrs, 1, kFrames);
    audio::BufferView<float> output(output_ptrs, 1, kFrames);
    audio::CallbackContext context{.sample_rate = 48'000.0,
                                   .buffer_size = kFrames};

    StandaloneRenderTestAccess::render(app, input, output, context);
    const auto transformed_peak = *std::max_element(rendered.begin(), rendered.end());
    REQUIRE(transformed_peak > kEffectPeak - 0.001f);
    REQUIRE(transformed_peak < kEffectPeak + 0.001f);

    cfg.route_test_signal_to_output = true;
    app.set_config(cfg);
    REQUIRE(app.test_input_host().inject_note({
                .kind = format::detail::StandaloneTestMidiKind::NoteOn,
                .channel = 1,
                .note = 60,
                .velocity = 100,
            }) == format::detail::StandaloneTestInputResult::Applied);
    rendered.fill(0.0f);
    StandaloneRenderTestAccess::render(app, input, output, context);
    const auto direct_peak = *std::max_element(rendered.begin(), rendered.end());
    REQUIRE(effect_fixture->saw_midi);
    REQUIRE(direct_peak > kInputPeak - 0.001f);
    REQUIRE(direct_peak < kInputPeak + 0.001f);
    REQUIRE(direct_peak < transformed_peak);
}
