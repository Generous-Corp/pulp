#pragma once

#include <pulp/format/processor.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace agent_toolkit_synth {

inline constexpr pulp::state::ParamID kLevel = 1;
inline constexpr pulp::state::ParamID kDetune = 2;

/// A one-voice sine synth: the newest held note plays at `kLevel`, offset by
/// `kDetune` semitones. Small on purpose, so the agent operations are the
/// interesting part of the example.
class Synth final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& context) override;
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override;

  private:
    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
    std::optional<std::uint8_t> note_;
};

} // namespace agent_toolkit_synth

std::unique_ptr<pulp::format::Processor> create_agent_toolkit_synth();
