#include "pulp_delay.hpp"

#include "delay_params.hpp"
#include "delay_time_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace pulp::examples::delay {
namespace {

constexpr float kControlRampSeconds = 0.005f;

float mono_routing_target(Routing routing) noexcept {
    return routing == Routing::mono ? 1.0f : 0.0f;
}

} // namespace

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
    routing_blend_.assign(scratch_size_, 0.0f);
    const auto sample_rate = static_cast<float>(context.sample_rate);
    mix_smoother_.set_ramp_time(kControlRampSeconds, sample_rate);
    routing_mono_smoother_.set_ramp_time(kControlRampSeconds, sample_rate);
    mix_smoother_.set_immediate(std::clamp(state().get_value(kMix) * 0.01f, 0.0f, 1.0f));
    routing_mono_smoother_.set_immediate(
        mono_routing_target(routing_from_param(state().get_value(kRouting))));
    const auto maximum_delay = static_cast<std::size_t>(std::ceil(context.sample_rate * 3.0)) + 4u;
    if (tempo_delay_.prepare(context.sample_rate, maximum_delay,
                             signal::TempoDelayWrapperInterpolation::lagrange3,
                             256) != signal::TempoDelayWrapperError::none) {
        prepared_ = false;
        return;
    }
    engines_.prepare(context.sample_rate, character_from_param(state().get_value(kCharacter)));
    prepared_ = true;
}

void PulpDelayProcessor::release() {
    tempo_delay_.reset();
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

    if (context.reset_requested) {
        tempo_delay_.reset();
        engines_.reset();
    }
    const auto character = character_from_param(state().get_value(kCharacter));
    const auto routing = routing_from_param(state().get_value(kRouting));
    const auto delay_inputs = delay_time_inputs_from_store(state(), context.tempo_bpm);
    const auto times = DelayTimeModel::derive(delay_inputs);
    const auto sample_rate = tempo_delay_.sample_rate();
    (void)tempo_delay_.set_delay_samples(static_cast<double>(times.left_ms) * sample_rate * 0.001,
                                         static_cast<double>(times.right_ms) * sample_rate * 0.001);
    (void)tempo_delay_.set_feedback(
        std::clamp(static_cast<double>(state().get_value(kFeedback)) * 0.01, 0.0,
                   signal::TempoDelayWrapper::kMaximumFeedback));
    (void)tempo_delay_.set_crossfeed(
        routing == Routing::ping_pong
            ? 1.0
            : std::clamp(static_cast<double>(state().get_value(kCrossfeed)) * 0.01, 0.0, 1.0));
    engines_.apply(engine_config(times, routing));
    engines_.request_character(character);

    const float mix = std::clamp(state().get_value(kMix) * 0.01f, 0.0f, 1.0f);
    if (mix != mix_smoother_.target())
        mix_smoother_.set_target(mix);
    const float mono_target = mono_routing_target(routing);
    if (mono_target != routing_mono_smoother_.target())
        routing_mono_smoother_.set_target(mono_target);
    auto* output_left = output.channel_ptr(0);
    auto* output_right = output.channel_ptr(1);
    const auto* input_left = input.channel_ptr(0);
    const auto* input_right = input.channel_ptr(1);

    std::size_t offset = 0;
    while (offset < samples) {
        const auto remaining = samples - offset;
        const int chunk = static_cast<int>(std::min(remaining, scratch_size_));
        process_chunk(output_left + offset, output_right + offset, input_left + offset,
                      input_right + offset, chunk);
        offset += static_cast<std::size_t>(chunk);
    }

    for (std::size_t channel = 2; channel < channels; ++channel) {
        std::copy_n(input.channel_ptr(channel), samples, output.channel_ptr(channel));
    }
}

CharacterEngineConfig PulpDelayProcessor::engine_config(const EffectiveDelayTimes& times,
                                                        Routing routing) const noexcept {
    const float mod_rate = std::clamp(state().get_value(kModRate), 0.05f, 10.0f);
    const float mod_rate_normalized = std::log(mod_rate / 0.05f) / std::log(10.0f / 0.05f);
    return {
        .time_ms = 1.0f,
        .right_time_ms = 1.0f,
        .time_offset = times.right_ratio,
        .right_uses_ratio = times.right_uses_ratio,
        .feedback = 0.0f,
        .crossfeed = 0.0f,
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
                                       int num_samples) noexcept {
    for (int i = 0; i < num_samples; ++i) {
        const auto index = static_cast<std::size_t>(i);
        dry_left_[index] = input_left[index];
        dry_right_[index] = input_right[index];
        const float mono = 0.5f * (input_left[index] + input_right[index]);
        const float mono_blend = routing_mono_smoother_.next();
        routing_blend_[index] = mono_blend;
        output_left[index] = input_left[index] + mono_blend * (mono - input_left[index]);
        output_right[index] = input_right[index] + mono_blend * (mono - input_right[index]);
    }

    // The wrapper owns the musical delay history and feedback route. The
    // character engine is deliberately kept at its minimum 1 ms line so it
    // colors the wrapper's wet signal without introducing a second long delay.
    (void)tempo_delay_.process(output_left, output_right, static_cast<std::size_t>(num_samples));
    engines_.process(output_left, output_right, num_samples, alternate_left_.data(),
                     alternate_right_.data());

    for (int i = 0; i < num_samples; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const float mix = mix_smoother_.next();
        const float stereo_left =
            dry_left_[index] + mix * (output_left[index] - dry_left_[index]);
        const float stereo_right =
            dry_right_[index] + mix * (output_right[index] - dry_right_[index]);
        const float dry_mono = 0.5f * (dry_left_[index] + dry_right_[index]);
        const float wet_mono = 0.5f * (output_left[index] + output_right[index]);
        const float mono_result = dry_mono + mix * (wet_mono - dry_mono);
        const float mono_blend = routing_blend_[index];
        output_left[index] = stereo_left + mono_blend * (mono_result - stereo_left);
        output_right[index] = stereo_right + mono_blend * (mono_result - stereo_right);
    }
}

std::unique_ptr<format::Processor> create_pulp_delay() {
    return std::make_unique<PulpDelayProcessor>();
}

} // namespace pulp::examples::delay
