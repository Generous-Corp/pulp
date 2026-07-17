#pragma once

// VaDrum -- a circuit-faithful TR-808 bass drum.
//
// One persistent resonator, monophonic by construction, with no voice
// allocation anywhere. Every note lands in the same still-ringing bridged-T,
// which is what makes repeated hits differ from one another instead of
// machine-gunning, and what makes the pitch sigh emerge rather than be drawn.
//
// A fixed one-in/one-out topology with no runtime routing, so this is a
// Processor rather than a graph. Complexity is not the criterion -- runtime
// routing is, and there is none here.
//
// Derived from the published circuit equations (Werner, Abel & Smith, DAFx-14)
// and the published schematic they adapt. See va_drum_voice.hpp.

#include "va_drum_voice.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <memory>

namespace pulp::examples {

enum VaDrumParams {
    kVaDrumTune = 0,
    kVaDrumDecay = 1,
    kVaDrumTone = 2,
    kVaDrumLevel = 3,
    kVaDrumPulseWidth = 4,
    kVaDrumAttackGate = 5,
    kVaDrumSigh = 6,
};

class VaDrum : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "VaDrum",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.va-drum",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Main Out", 2, false}},
            .accepts_midi = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Every control is a component value or a knob position on one, which
        // is what makes the circuit bendable: a "mod" is an edit to an R or a C
        // in the same equations the stock sound comes out of.
        store.add_parameter({.id = kVaDrumTune,
                             .name = "Tune",
                             .unit = "x",
                             .range = {0.5f, 2.0f, 1.0f, 0.01f}});
        store.add_parameter({.id = kVaDrumDecay,
                             .name = "Decay",
                             .unit = "%",
                             .range = {0.0f, 100.0f, 50.0f, 0.1f}});
        store.add_parameter({.id = kVaDrumTone,
                             .name = "Tone",
                             .unit = "%",
                             .range = {0.0f, 100.0f, 50.0f, 0.1f}});
        store.add_parameter({.id = kVaDrumLevel,
                             .name = "Level",
                             .unit = "%",
                             .range = {0.0f, 100.0f, 80.0f, 0.1f}});
        // The paper lists pulse width and attack timing among the circuit's
        // bends; C38 is what moves the latter in hardware.
        store.add_parameter({.id = kVaDrumPulseWidth,
                             .name = "Pulse Width",
                             .unit = "ms",
                             .range = {0.1f, 10.0f, 1.0f, 0.01f}});
        store.add_parameter({.id = kVaDrumAttackGate,
                             .name = "Attack Gate",
                             .unit = "ms",
                             .range = {0.0f, 20.0f, 5.4f, 0.1f}});
        // Cutting the R161 leakage path removes the sigh entirely.
        store.add_parameter({.id = kVaDrumSigh,
                             .name = "Pitch Sigh",
                             .unit = "",
                             .range = {0.0f, 1.0f, 1.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext& ctx) override {
        voice_.prepare(ctx.sample_rate);
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        // A timeline discontinuity is the one time the ring should be cleared:
        // the host has jumped somewhere else and the tail belongs to a moment
        // that is no longer playing. This is categorically different from a
        // trigger, which must never clear it.
        if (ctx.should_reset_dsp_state()) voice_.reset();

        // The Tune and Decay knobs pass through the reference calibration so the
        // stock positions land on the measured TR-808 curve; every other control
        // is the raw component value.
        voice_.set_tune(state().get_value(kVaDrumTune) * kReferenceTuneTrim);
        voice_.set_decay(reference_decay_taper(state().get_value(kVaDrumDecay) / 100.0));
        voice_.set_tone(state().get_value(kVaDrumTone) / 100.0);
        voice_.set_level(state().get_value(kVaDrumLevel) / 100.0);
        voice_.set_pulse_width_s(state().get_value(kVaDrumPulseWidth) / 1000.0);
        voice_.set_attack_gate_s(state().get_value(kVaDrumAttackGate) / 1000.0);
        voice_.set_sigh_enabled(state().get_value(kVaDrumSigh) >= 0.5f);

        auto left = output.channel(0);
        auto right = output.channel(output.num_channels() > 1 ? 1 : 0);
        const auto n_samples = static_cast<int32_t>(left.size());

        // Walk the block sample by sample so a trigger lands on the sample the
        // host timestamped it for. A drum's attack is a handful of
        // milliseconds, so block-quantized triggering is audible as timing jitter.
        int32_t next_event = 0;
        const auto event_count = static_cast<int32_t>(midi_in.size());

        for (int32_t i = 0; i < n_samples; ++i) {
            while (next_event < event_count) {
                const auto& event = midi_in[static_cast<std::size_t>(next_event)];
                if (event.sample_offset > i) break;
                // Note-off is meaningless here: the drum rings until it stops,
                // and there is nothing to release.
                if (event.is_note_on() && event.velocity() > 0) {
                    voice_.trigger(accent_volts(event.velocity()));
                }
                ++next_event;
            }

            const auto sample =
                static_cast<float>(voice_.process() * kVoltsToSample * kReferenceLevelTrim);
            left[static_cast<std::size_t>(i)] = sample;
            if (output.num_channels() > 1) right[static_cast<std::size_t>(i)] = sample;
        }

        voice_.snap_denormals();
    }

    BassDrumVoice& voice() noexcept { return voice_; }

private:
    /// Velocity arrives as accent, and accent is the trigger pulse's amplitude:
    /// the CPU's accent line swings between roughly 4 V and 14 V depending on
    /// the global accent pot. This is the strike force, not a level -- the
    /// difference is that the pulse shaper's falling edge stays pinned at one
    /// diode drop while its rising edge scales, so a hard hit is a different
    /// timbre and a different pitch trajectory rather than a louder soft one.
    static double accent_volts(uint8_t velocity) noexcept {
        constexpr double min_v = 4.0;
        constexpr double max_v = 14.0;
        return min_v + (max_v - min_v) * (static_cast<double>(velocity) / 127.0);
    }

    /// Volts at the output buffer to full scale. The circuit's own output level
    /// is set by whatever it drives, so this is a unit conversion against the
    /// supply rails rather than anything derived from the schematic.
    static constexpr double kVoltsToSample = 1.0 / 15.0;

    /// Loudness match to the reference instrument: at the stock knob positions
    /// the raw circuit output peaks about 3.5 dB above the reference render, so
    /// the default sound sits on it rather than running hot. This is a level
    /// calibration, not a change to the circuit; the Level knob still scales
    /// around it.
    static constexpr double kReferenceLevelTrim = 0.667;

    BassDrumVoice voice_{};
};

inline std::unique_ptr<format::Processor> create_va_drum() {
    return std::make_unique<VaDrum>();
}

}  // namespace pulp::examples
