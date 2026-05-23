#include "sine_synth.hpp"

#include <cmath>

namespace pulp::examples::ios_synth {

using namespace pulp::format;
using namespace pulp::state;
using namespace pulp::audio;
using namespace pulp::midi;

constexpr double kTwoPi = 6.283185307179586;

PluginDescriptor SineSynth::descriptor() const {
    PluginDescriptor d;
    d.name = "Pulp Sine Synth";
    d.manufacturer = "Pulp";
    d.bundle_id = "com.pulp.examples.sinesynth";
    d.version = "0.1.0";
    d.category = PluginCategory::Instrument;
    d.accepts_midi = true;
    d.produces_midi = false;
    // Instruments: no audio input bus; stereo main output.
    d.input_buses.clear();
    d.output_buses = {{"Main Out", 2, false}};
    return d;
}

void SineSynth::define_parameters(StateStore& store) {
    freq_param_id_  = store.define(ParamInfo{"freq",  "Frequency", 110.0f, 30.0f, 4000.0f});
    level_param_id_ = store.define(ParamInfo{"level", "Level",     0.5f,   0.0f, 1.0f});
}

void SineSynth::prepare(const PrepareContext& ctx) {
    sample_rate_ = ctx.sample_rate > 0 ? ctx.sample_rate : 48000.0;
    phase_ = 0.0;
    gate_ = 0.0f;
    gate_target_ = 0.0f;
}

void SineSynth::process(BufferView<float>& out,
                        const BufferView<const float>& /*in*/,
                        MidiBuffer& midi_in,
                        MidiBuffer& /*midi_out*/,
                        const ProcessContext& ctx) {
    // Extremely small keyboard: any note-on opens the gate; any note-off
    // closes it. This is a scaffold example, not a playable synth.
    for (const auto& ev : midi_in) {
        const auto& msg = ev.message;
        if (msg.isNoteOn())  gate_target_ = 1.0f;
        if (msg.isNoteOff()) gate_target_ = 0.0f;
    }

    const float freq  = state().get_value(freq_param_id_);
    const float level = state().get_value(level_param_id_);
    const double phase_inc = kTwoPi * freq / sample_rate_;
    const float gate_step  = 0.002f; // ~10 ms AR at 48 kHz

    const int n = ctx.num_samples;
    float* const L = out.channel_data(0);
    float* const R = out.num_channels() > 1 ? out.channel_data(1) : nullptr;

    for (int i = 0; i < n; ++i) {
        gate_ += (gate_target_ - gate_) * gate_step;
        const float s = static_cast<float>(std::sin(phase_)) * gate_ * level;
        phase_ += phase_inc;
        if (phase_ >= kTwoPi) phase_ -= kTwoPi;
        L[i] = s;
        if (R) R[i] = s;
    }
}

} // namespace pulp::examples::ios_synth

// Factory registration moved to src/au_v3_entry.cpp (PULP_AUV3_PLUGIN) so the
// same wiring matches the macOS AU v3 + CLAP + AU v2 entry pattern.
