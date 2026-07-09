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
#include <tuple>
#include <cstddef>
#include <memory>
#include <utility>

namespace pulp::examples::brew {

class LfoProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kBeatsPerCycle = 1,
        kPhaseDegrees = 2,
        kSine = 3,
        kTriangle = 4,
        kSaw = 5,
        kSquare = 6,
        kPulseWidth = 7,
        kRandom = 8,
        kAsymmetry = 9,
        kOffset = 10,
        kSeed = 11,
        kOutputScale = 12,
        kInvert = 13,
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
        // Beats per cycle: 4 is one cycle per bar of 4/4, 0.25 is four per beat.
        store.add_parameter({.id = kBeatsPerCycle,
                             .name = "Rate",
                             .unit = "beats",
                             .range = {0.0625f, 16.0f, 1.0f, 0.0625f}});
        store.add_parameter({.id = kPhaseDegrees,
                             .name = "Phase",
                             .unit = "deg",
                             .range = {0.0f, 360.0f, 0.0f, 1.0f}});

        // Four depths, summed. Bipolar, so a shape can be subtracted as easily as
        // added. Sine defaults to full and the rest to zero, which reproduces the
        // single-shape selector this replaced.
        for (auto [id, name, def] :
             {std::tuple{kSine, "Sine", 1.0f}, std::tuple{kTriangle, "Triangle", 0.0f},
              std::tuple{kSaw, "Saw", 0.0f}, std::tuple{kSquare, "Square", 0.0f}})
            store.add_parameter({.id = id, .name = name, .unit = "",
                                 .range = {-1.0f, 1.0f, def, 0.001f}});

        store.add_parameter({.id = kPulseWidth,
                             .name = "Pulse Width",
                             .unit = "",
                             .range = {0.01f, 0.99f, 0.5f, 0.001f}});
        // A sample-and-hold: one level per cycle, held flat. A hash of the cycle
        // index, so a bounce lands identically. See brew/random.hpp.
        store.add_parameter({.id = kRandom,
                             .name = "Random",
                             .unit = "",
                             .range = {-1.0f, 1.0f, 0.0f, 0.001f}});
        // Where the waveform's centre falls in time. A pulse-width control
        // generalized to every shape.
        store.add_parameter({.id = kAsymmetry,
                             .name = "Asymmetry",
                             .unit = "",
                             .range = {0.01f, 0.99f, 0.5f, 0.001f}});
        // A constant offset. Set it to +1 with a half-scale mix and the output is
        // unipolar, which is what a VCA or an envelope-depth input wants.
        store.add_parameter({.id = kOffset,
                             .name = "Offset",
                             .unit = "",
                             .range = {-1.0f, 1.0f, 0.0f, 0.001f}});
        store.add_parameter({.id = kSeed,
                             .name = "Seed",
                             .unit = "",
                             .range = {0.0f, 255.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kOutputScale,
                             .name = "Output Scale",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f}});
        store.add_parameter({.id = kInvert,
                             .name = "Invert",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
    }

    /// A snapshot of the mix, taken once per block rather than once per sample.
    /// The editor's scope reads the same struct, so the picture cannot drift.
    [[nodiscard]] LfoMix mix() const noexcept {
        return {
            .sine = state().get_value(kSine),
            .triangle = state().get_value(kTriangle),
            .saw = state().get_value(kSaw),
            .square = state().get_value(kSquare),
            .random = state().get_value(kRandom),
            .pulse_width = state().get_value(kPulseWidth),
            .asymmetry = state().get_value(kAsymmetry),
            .offset = state().get_value(kOffset),
            .seed = static_cast<std::uint32_t>(state().get_value(kSeed)),
        };
    }

    void prepare(const format::PrepareContext&) override {}

    /// The size this editor actually needs. Without this override a host opens
    /// the plug-in at Processor's 400x300 default, and the layout is laid out to
    /// a geometry the editor was never checked against. The scope, three knobs, and the two toggles.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 560};
    }

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
        return value_at(mix(), position_beats, quadrature);
    }

    /// The overload the audio callback uses, with the mix hoisted out of the
    /// per-sample loop.
    [[nodiscard]] float value_at(const LfoMix& m, double position_beats,
                                 double quadrature = 0.0) const noexcept {
        const double rate = static_cast<double>(state().get_value(kBeatsPerCycle));
        const double phase = lfo_phase(
            position_beats, rate,
            static_cast<double>(state().get_value(kPhaseDegrees)) / 360.0 +
                quadrature);
        // The sample-and-hold is keyed on the cycle the *quadrature* phase sits
        // in, not the main one, or the two outputs would hold different levels
        // across a cycle boundary and the circle they trace would tear.
        const std::int64_t cycle = lfo_cycle(position_beats + quadrature * rate, rate);
        return resolve_output(lfo_mix_value(m, phase, cycle),
                              state().get_value(kOutputScale),
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
        const LfoMix m = mix();
        for (int n = 0; n < frames; ++n) {
            const double at = ctx.position_beats + bps * static_cast<double>(n);
            if (main) main[n] = value_at(m, at);
            if (quad) quad[n] = value_at(m, at, kQuadratureOffset);
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
