#include "pulp_delay.hpp"

#include "delay_params.hpp"
#include "delay_time_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::examples::delay {

format::PluginDescriptor PulpDelayProcessor::descriptor() const {
    return {
        .name = "Pulp Delay",
        .manufacturer = "Pulp",
        .bundle_id = "com.pulp.delay",
        .version = "1.0.0",
        .category = format::PluginCategory::Effect,
        .input_buses = {{"Audio In", 2}},
        .output_buses = {{"Audio Out", 2}},
    };
}

void PulpDelayProcessor::define_parameters(state::StateStore& store) {
    define_delay_parameters(store);
}

void PulpDelayProcessor::prepare(const format::PrepareContext& context) {
    scratch_size_ = static_cast<std::size_t>(std::max(1, context.max_buffer_size));
    dry_left_.assign(scratch_size_, 0.0f);
    dry_right_.assign(scratch_size_, 0.0f);
    alternate_left_.assign(scratch_size_, 0.0f);
    alternate_right_.assign(scratch_size_, 0.0f);
    engines_.prepare(context.sample_rate, character_from_param(state().get_value(kCharacter)));
    prepared_ = true;
}

void PulpDelayProcessor::release() {
    engines_.reset();
    prepared_ = false;
}

void PulpDelayProcessor::process(audio::BufferView<float>& output,
                                 const audio::BufferView<const float>& input, midi::MidiBuffer&,
                                 midi::MidiBuffer&, const format::ProcessContext& context) {
    const auto channels = std::min(output.num_channels(), input.num_channels());
    const auto samples = std::min(output.num_samples(), input.num_samples());
    if (channels < 2 || samples == 0 || !prepared_ || scratch_size_ == 0) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto* destination = output.channel_ptr(channel);
            const auto* source = input.channel_ptr(channel);
            std::copy_n(source, samples, destination);
        }
        return;
    }

    const auto character = character_from_param(state().get_value(kCharacter));
    const auto routing = routing_from_param(state().get_value(kRouting));
    const auto times = DelayTimeModel::derive(time_inputs(context));
    engines_.request_character(character);
    engines_.apply(engine_config(times, routing));

    const float mix = std::clamp(state().get_value(kMix) * 0.01f, 0.0f, 1.0f);
    auto* output_left = output.channel_ptr(0);
    auto* output_right = output.channel_ptr(1);
    const auto* input_left = input.channel_ptr(0);
    const auto* input_right = input.channel_ptr(1);

    std::size_t offset = 0;
    while (offset < samples) {
        const auto remaining = samples - offset;
        const int chunk = static_cast<int>(std::min(remaining, scratch_size_));
        process_chunk(output_left + offset, output_right + offset, input_left + offset,
                      input_right + offset, chunk, mix, routing);
        offset += static_cast<std::size_t>(chunk);
    }

    for (std::size_t channel = 2; channel < channels; ++channel) {
        std::copy_n(input.channel_ptr(channel), samples, output.channel_ptr(channel));
    }
}

DelayTimeInputs
PulpDelayProcessor::time_inputs(const format::ProcessContext& context) const noexcept {
    return {
        .time_ms = state().get_value(kTime),
        .sync = state().get_value(kSync) >= 0.5f,
        .division = division_index_from_param(state().get_value(kDivision)),
        .link = state().get_value(kLink) >= 0.5f,
        .offset_mode = offset_mode_from_param(state().get_value(kOffsetMode)),
        .time_offset = state().get_value(kTimeOffset),
        .offset_ms = state().get_value(kOffsetMs),
        .time_right_ms = state().get_value(kTimeRight),
        .division_right = division_index_from_param(state().get_value(kDivisionRight)),
        .routing = routing_from_param(state().get_value(kRouting)),
        .tempo_bpm = context.tempo_bpm,
    };
}

CharacterEngineConfig PulpDelayProcessor::engine_config(const EffectiveDelayTimes& times,
                                                        Routing routing) const noexcept {
    const float mod_rate = std::clamp(state().get_value(kModRate), 0.05f, 10.0f);
    const float mod_rate_normalized = std::log(mod_rate / 0.05f) / std::log(10.0f / 0.05f);
    return {
        .time_ms = times.left_ms,
        .right_time_ms = times.right_ms,
        .time_offset = times.right_ratio,
        .right_uses_ratio = times.right_uses_ratio,
        .feedback = state().get_value(kFeedback) * 0.01f,
        .crossfeed = routing == Routing::ping_pong ? 1.0f : state().get_value(kCrossfeed) * 0.01f,
        .character_amount = state().get_value(kCharacterAmount) * 0.01f,
        .diffusion = state().get_value(kDiffusion) * 0.01f,
        .duck = state().get_value(kDuck) * 0.01f,
        .low_cut_hz = state().get_value(kLowCut),
        .high_cut_hz = state().get_value(kHighCut),
        .low_cut_resonance = state().get_value(kLowCutResonance),
        .high_cut_resonance = state().get_value(kHighCutResonance),
        .mod_rate_normalized = mod_rate_normalized,
        .mod_depth = state().get_value(kModDepth) * 0.01f,
        .freeze = state().get_value(kFreeze) >= 0.5f,
        .reverse = state().get_value(kReverse) >= 0.5f,
    };
}

void PulpDelayProcessor::process_chunk(float* output_left, float* output_right,
                                       const float* input_left, const float* input_right,
                                       int num_samples, float mix, Routing routing) noexcept {
    for (int i = 0; i < num_samples; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (routing == Routing::mono) {
            const float mono = 0.5f * (input_left[index] + input_right[index]);
            dry_left_[index] = mono;
            dry_right_[index] = mono;
            output_left[index] = mono;
            output_right[index] = mono;
        } else {
            dry_left_[index] = input_left[index];
            dry_right_[index] = input_right[index];
            output_left[index] = input_left[index];
            output_right[index] = input_right[index];
        }
    }

    engines_.process(output_left, output_right, num_samples, alternate_left_.data(),
                     alternate_right_.data());

    for (int i = 0; i < num_samples; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (routing == Routing::mono) {
            const float wet = 0.5f * (output_left[index] + output_right[index]);
            const float result = dry_left_[index] + mix * (wet - dry_left_[index]);
            output_left[index] = result;
            output_right[index] = result;
        } else {
            output_left[index] = dry_left_[index] + mix * (output_left[index] - dry_left_[index]);
            output_right[index] =
                dry_right_[index] + mix * (output_right[index] - dry_right_[index]);
        }
    }
}

std::unique_ptr<format::Processor> create_pulp_delay() {
    return std::make_unique<PulpDelayProcessor>();
}

} // namespace pulp::examples::delay
