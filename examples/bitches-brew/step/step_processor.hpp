#pragma once

// Step LFO — an eight-step pattern, derived from the host's position.
//
// Two outputs: the stepped control voltage on channel 0, and a gate on channel 1
// that goes high for the gated part of every step. The gate is what lets a step
// pattern drive an envelope downstream rather than only a level.
//
// Like the LFO and the clock, the step index is a pure function of the position:
// bar 57 always plays the same step, whatever the playhead did to get there. That
// rule forces the design of `Random`, because a running generator drifts on a
// locate and renders differently every bounce. So `Random` is a hash of the
// absolute step index — deterministic, unbounded, non-repeating. Change the seed
// to reroll it; pressing play again will not.
//
// Not implemented yet, and named so nobody assumes otherwise: the reference's
// `Roll` (write a random pattern *into* the steps, where it can be hand-edited and
// stored in the preset), its `DAC Bits` quantizer, its trigger-advance sync modes,
// and its start/length/end pattern bounds. The step editor here is draggable but
// has none of the modifier-key refinements.

#include <brew/clock.hpp>  // beats_per_sample
#include <brew/cv.hpp>
#include <brew/step.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::brew {

class StepProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kRate = 1,
        kSpeedMode = 2,
        kLength = 3,
        kGlide = 4,
        kRandom = 5,
        kSeed = 6,
        kOutputScale = 7,
        kInvert = 8,
        /// The eight step levels occupy 16..23, leaving room below for controls.
        kGate = 9,
        kStep1 = 16,
    };

    [[nodiscard]] static constexpr state::ParamID step_param(int i) noexcept {
        return static_cast<state::ParamID>(kStep1 + i);
    }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Step LFO",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.step",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Steps / Gate", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Read as the whole pattern's period in `cycle`, or one step's in `step`.
        store.add_parameter({.id = kRate,
                             .name = "Rate",
                             .unit = "beats",
                             .range = {0.0625f, 16.0f, 4.0f, 0.0625f}});
        store.add_parameter({.id = kSpeedMode,
                             .name = "Rate Per Step",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        store.add_parameter({.id = kLength,
                             .name = "Length",
                             .unit = "steps",
                             .range = {1.0f, static_cast<float>(kMaxSequencerSteps),
                                       8.0f, 1.0f}});
        // Fraction of a step spent sliding into it. Zero is a hard edge.
        store.add_parameter({.id = kGlide,
                             .name = "Glide",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.001f}});
        // The fraction of each step that carries its value. At 1.0 nothing is
        // punched out and the gate never falls; below it, every step grows a
        // rising edge for an envelope generator to fire on.
        store.add_parameter({.id = kGate,
                             .name = "Gate",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 0.001f}});
        // A bounded random offset added to each step, keyed on the absolute step
        // index. At zero the pattern is exactly what the editor shows.
        store.add_parameter({.id = kRandom,
                             .name = "Random",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 0.001f}});
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

        // A gentle rising ramp, so an unedited instance does something audible on
        // a scope rather than sitting flat and looking broken.
        for (int i = 0; i < kMaxSequencerSteps; ++i) {
            const float def =
                -1.0f + 2.0f * static_cast<float>(i) /
                            static_cast<float>(kMaxSequencerSteps - 1);
            store.add_parameter({.id = step_param(i),
                                 .name = "Step " + std::to_string(i + 1),
                                 .unit = "",
                                 .range = {-1.0f, 1.0f, def, 0.001f}});
        }
    }

    void prepare(const format::PrepareContext&) override {}

    /// The step editor, two rows of controls, and the two caption lines.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 412};
    }

    std::unique_ptr<view::View> create_view() override;

    [[nodiscard]] int length() const noexcept {
        return std::clamp(static_cast<int>(std::lround(state().get_value(kLength))),
                          1, kMaxSequencerSteps);
    }

    /// Beats occupied by one step under the current settings.
    [[nodiscard]] double step_beats() const noexcept {
        return beats_per_step(speed_mode_from_param(state().get_value(kSpeedMode)),
                              static_cast<double>(state().get_value(kRate)), length());
    }

    /// The level the pattern holds at an absolute step, before glide and the
    /// output stage. Pure, and shared by process(), the editor, and the tests.
    [[nodiscard]] float level_at(std::int64_t abs_step) const noexcept {
        const int idx = static_cast<int>(wrap_index(abs_step, length()));
        return step_value(state().get_value(step_param(idx)), abs_step,
                          state().get_value(kRandom),
                          static_cast<std::uint32_t>(state().get_value(kSeed)));
    }

    /// How much of each step carries its value; the rest is silent.
    [[nodiscard]] double gate_fraction() const noexcept {
        return static_cast<double>(state().get_value(kGate));
    }

    /// The value emitted at a beat position, after glide, the gate, and the
    /// output stage.
    [[nodiscard]] float value_at(double position_beats) const noexcept {
        const double bps_step = step_beats();
        if (!(bps_step > 0.0)) return resolve_output(0.0f, scale(), inverted());
        const std::int64_t abs = absolute_step(position_beats, bps_step);
        const double frac = step_fraction(position_beats, bps_step);
        const float v = glide_toward(level_at(abs - 1), level_at(abs), frac,
                                     static_cast<double>(state().get_value(kGlide)));
        // Gated before the output stage, so `Out` and `Invert` act on the gap the
        // same way they act on the note — a gap is a voltage like any other.
        return resolve_output(step_gated(v, frac, gate_fraction()), scale(), inverted());
    }

    /// Which step of the pattern is playing, or -1 when nothing is. The editor
    /// highlights it; a negative value hides the highlight rather than lying.
    [[nodiscard]] int display_step() const noexcept {
        return display_step_.load(std::memory_order_relaxed);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t channels = output.num_channels();
        const int frames = static_cast<int>(output.num_samples());
        if (channels == 0 || frames <= 0) return;

        float* cv = output.channel_ptr(0);
        float* gate = channels > 1 ? output.channel_ptr(1) : nullptr;

        // Bypass stops driving the patch: hold both outputs at zero rather than
        // freeze them at whatever level the pattern happened to reach.
        if (ctx.is_bypassed) {
            for (int n = 0; n < frames; ++n) {
                if (cv) cv[n] = 0.0f;
                if (gate) gate[n] = 0.0f;
            }
            display_step_.store(-1, std::memory_order_relaxed);
            return;
        }

        const double bps = ctx.is_playing
                               ? beats_per_sample(ctx.tempo_bpm, ctx.sample_rate)
                               : 0.0;
        const double bps_step = step_beats();
        const double gate_open = gate_fraction();
        const float high = resolve_output(kFullScale, scale(), inverted());
        const float low = resolve_output(0.0f, scale(), inverted());

        std::int64_t last_abs = 0;
        for (int n = 0; n < frames; ++n) {
            const double pos = ctx.position_beats + bps * n;
            if (cv) cv[n] = value_at(pos);
            if (gate) {
                const bool on = bps_step > 0.0 &&
                                step_gate_open(step_fraction(pos, bps_step), gate_open);
                gate[n] = on ? high : low;
            }
            last_abs = bps_step > 0.0 ? absolute_step(pos, bps_step) : 0;
        }

        display_step_.store(
            bps_step > 0.0 ? static_cast<int>(wrap_index(last_abs, length())) : -1,
            std::memory_order_relaxed);
    }

private:
    [[nodiscard]] float scale() const noexcept {
        return state().get_value(kOutputScale);
    }
    [[nodiscard]] bool inverted() const noexcept {
        return as_toggle(state().get_value(kInvert));
    }

    std::atomic<int> display_step_{-1};
};

inline std::unique_ptr<format::Processor> create_step() {
    return std::make_unique<StepProcessor>();
}

}  // namespace pulp::examples::brew
