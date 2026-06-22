#pragma once

// Minimal sine-wave instrument for the iOS AUv3 example. One
// "frequency" parameter plus a gate that's opened by any note-on MIDI
// event. Lives in the example tree on purpose — it is NOT part of the
// framework API.

#include <pulp/format/processor.hpp>

namespace pulp::examples::ios_synth {

class SineSynth : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& ctx) override;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& ctx) override;

private:
    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
    float  gate_ = 0.0f;   // 0..1, ramped towards target on note on/off
    float  gate_target_ = 0.0f;
    int    freq_param_id_ = -1;
    int    level_param_id_ = -1;
};

} // namespace pulp::examples::ios_synth
