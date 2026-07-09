#pragma once

// LFO — a tempo-locked modulation source, and its quadrature partner.
//
// Two outputs: the shape on channel 0, and the same shape a quarter cycle ahead
// on channel 1. Patched into two CV inputs they trace a circle, which is how you
// drive a two-axis modulation (a filter's cutoff and resonance, a panner's X and
// Y) from a single oscillator.
//
// The phase is derived from the host's position, never accumulated — see
// brew/lfo.hpp. So the modulation is bit-identical across bounces, lands where
// the timeline says it should after a locate, and never drifts against the host
// over a long session.
//
// Per-sample, not per-block. A block-rate LFO steps its value 512 samples at a
// time, and a stepped control voltage is an audible zipper on anything it drives.

#include <brew/clock.hpp>  // beats_per_sample
#include <brew/cv.hpp>
#include <brew/lfo.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>

namespace pulp::examples::brew {

class LfoProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kWaveform = 1,
        kBeatsPerCycle = 2,
        kPhaseDegrees = 3,
        kUnipolar = 4,
        kOutputScale = 5,
        kInvert = 6,
    };

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "LFO",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.lfo",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"LFO / Quadrature", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kWaveform,
                             .name = "Waveform",
                             .unit = "",
                             .range = {0.0f, static_cast<float>(kWaveformCount - 1),
                                       0.0f, 1.0f}});
        // Beats per cycle: 4 is one cycle per bar of 4/4, 0.25 is four per beat.
        store.add_parameter({.id = kBeatsPerCycle,
                             .name = "Rate",
                             .unit = "beats",
                             .range = {0.0625f, 16.0f, 1.0f, 0.0625f}});
        store.add_parameter({.id = kPhaseDegrees,
                             .name = "Phase",
                             .unit = "deg",
                             .range = {0.0f, 360.0f, 0.0f, 1.0f}});
        // A VCA or an envelope-depth input wants positive voltage only; a bipolar
        // LFO would waste half its travel and silence half the cycle.
        store.add_parameter({.id = kUnipolar,
                             .name = "Unipolar",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f}});
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext&) override {}

    /// Defined in lfo_view.cpp so the audio translation units never see the
    /// view stack.
    std::unique_ptr<view::View> create_view() override;

    /// Phase at the end of the last rendered block, for the editor's scope
    /// marker. Written once per block on the audio thread with relaxed ordering:
    /// a marker one frame stale is invisible, and synchronizing for it would not
    /// be. Negative means "not running" — the marker hides rather than lying.
    [[nodiscard]] float display_phase() const noexcept {
        return display_phase_.load(std::memory_order_relaxed);
    }

    /// The value the plug-in emits at a given beat position, after the shared
    /// output stage. Pure, and shared by process() and the tests so a test cannot
    /// silently diverge from the DSP.
    [[nodiscard]] float value_at(double position_beats,
                                 double quadrature = 0.0) const noexcept {
        const auto wave = waveform_from_param(state().get_value(kWaveform));
        const double phase = lfo_phase(
            position_beats, static_cast<double>(state().get_value(kBeatsPerCycle)),
            static_cast<double>(state().get_value(kPhaseDegrees)) / 360.0 +
                quadrature);

        float v = lfo_shape(wave, phase);
        if (as_toggle(state().get_value(kUnipolar))) v = to_unipolar(v);
        return resolve_output(v, state().get_value(kOutputScale),
                              as_toggle(state().get_value(kInvert)));
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t channels = output.num_channels();
        const int frames = static_cast<int>(output.num_samples());
        if (channels == 0 || frames <= 0) return;

        float* main = output.channel_ptr(0);
        float* quad = channels > 1 ? output.channel_ptr(1) : nullptr;

        // Bypass means stop driving the patch: hold the outputs at zero rather
        // than freeze them at whatever voltage the cycle happened to reach.
        if (ctx.is_bypassed) {
            for (int n = 0; n < frames; ++n) {
                if (main) main[n] = 0.0f;
                if (quad) quad[n] = 0.0f;
            }
            display_phase_.store(-1.0f, std::memory_order_relaxed);
            return;
        }

        // Walk the block per sample. The position advances even when the host is
        // stopped only if the host says so — a stopped transport holds the LFO
        // where the playhead sits, which is what a user parked on a beat expects
        // to see on a scope.
        const double bps = ctx.is_playing
                               ? beats_per_sample(ctx.tempo_bpm, ctx.sample_rate)
                               : 0.0;
        for (int n = 0; n < frames; ++n) {
            const double at = ctx.position_beats + bps * static_cast<double>(n);
            if (main) main[n] = value_at(at);
            if (quad) quad[n] = value_at(at, kQuadratureOffset);
        }

        const double end = ctx.position_beats + bps * static_cast<double>(frames - 1);
        display_phase_.store(
            static_cast<float>(lfo_phase(
                end, static_cast<double>(state().get_value(kBeatsPerCycle)),
                static_cast<double>(state().get_value(kPhaseDegrees)) / 360.0)),
            std::memory_order_relaxed);
    }

    void process(format::ProcessBuffers& audio,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const format::ProcessContext& ctx) override {
        if (auto* out = audio.main_output()) {
            audio::BufferView<const float> unused_input;
            process(*out, unused_input, midi_in, midi_out, ctx);
        }
    }

private:
    std::atomic<float> display_phase_{-1.0f};
};

inline std::unique_ptr<format::Processor> create_lfo() {
    return std::make_unique<LfoProcessor>();
}

}  // namespace pulp::examples::brew
