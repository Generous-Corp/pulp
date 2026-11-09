#include "agent_toolkit_synth.hpp"

#include <cmath>
#include <numbers>

namespace agent_toolkit_synth {

pulp::format::PluginDescriptor Synth::descriptor() const {
    pulp::format::PluginDescriptor d;
    d.name = "Agent Toolkit Synth";
    d.manufacturer = "Pulp Examples";
    d.bundle_id = "com.example.agent-toolkit-synth";
    d.version = "1.0.0";
    d.category = pulp::format::PluginCategory::Instrument;
    d.accepts_midi = true;
    d.output_buses = {{"Out", 2, false}};
    return d;
}

void Synth::define_parameters(pulp::state::StateStore& store) {
    pulp::state::ParamInfo level;
    level.id = kLevel;
    level.name = "Level";
    level.range = {0.0f, 1.0f, 0.5f};
    store.add_parameter(level);
    pulp::state::ParamInfo detune;
    detune.id = kDetune;
    detune.name = "Detune";
    detune.unit = "st";
    detune.range = {-12.0f, 12.0f, 0.0f};
    store.add_parameter(detune);
}

void Synth::prepare(const pulp::format::PrepareContext& context) {
    sample_rate_ = context.sample_rate;
    phase_ = 0.0;
    note_.reset();
}

void Synth::process(pulp::audio::BufferView<float>& output,
                    const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer& midi_in,
                    pulp::midi::MidiBuffer&, const pulp::format::ProcessContext& context) {
    for (const auto& event : midi_in) {
        if (event.is_note_on() && event.velocity() > 0)
            note_ = event.note();
        else if ((event.is_note_off() || event.is_note_on()) && note_ == event.note())
            note_.reset();
    }
    const auto level = state().get_value(kLevel);
    const auto pitch = note_ ? static_cast<double>(*note_) + state().get_value(kDetune) : 0.0;
    const auto step =
        2.0 * std::numbers::pi * 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0) / sample_rate_;
    for (int frame = 0; frame < context.num_samples; ++frame) {
        const auto sample = note_ ? static_cast<float>(level * std::sin(phase_)) : 0.0f;
        if (note_)
            phase_ = std::fmod(phase_ + step, 2.0 * std::numbers::pi);
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            output.channel(channel)[static_cast<std::size_t>(frame)] = sample;
    }
}

} // namespace agent_toolkit_synth

std::unique_ptr<pulp::format::Processor> create_agent_toolkit_synth() {
    return std::make_unique<agent_toolkit_synth::Synth>();
}
