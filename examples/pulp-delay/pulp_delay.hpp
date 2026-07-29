#pragma once

#include "character_engine_bank.hpp"
#include "delay_time_model.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/smoothed_value.hpp>

#include <memory>
#include <vector>

namespace pulp::examples::delay {

class PulpDelayProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override;
    void define_parameters(state::StateStore& store) override;
    std::unique_ptr<view::View> create_view() override;
    void prepare(const format::PrepareContext& context) override;
    void release() override;

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer& midi_input, midi::MidiBuffer& midi_output,
                 const format::ProcessContext& context) override;

  private:
    CharacterEngineConfig engine_config(const EffectiveDelayTimes& times,
                                        Routing routing) const noexcept;
    void process_chunk(float* output_left, float* output_right, const float* input_left,
                       const float* input_right, int num_samples) noexcept;

    CharacterEngineBank engines_;
    std::vector<float> dry_left_;
    std::vector<float> dry_right_;
    std::vector<float> alternate_left_;
    std::vector<float> alternate_right_;
    std::vector<float> routing_blend_;
    signal::SmoothedValue<float> mix_smoother_;
    signal::SmoothedValue<float> routing_mono_smoother_;
    std::size_t scratch_size_ = 0;
    bool prepared_ = false;
};

std::unique_ptr<format::Processor> create_pulp_delay();

} // namespace pulp::examples::delay
