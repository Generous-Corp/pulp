#include <pulp/audio/sample_voice_renderer.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

namespace {

bool positive_finite(double value) noexcept {
    return value > 0.0 && std::isfinite(value);
}

void clear_destination(BufferView<float> destination,
                       std::uint64_t frames) noexcept {
    const auto frame_count = std::min<std::uint64_t>(
        frames,
        static_cast<std::uint64_t>(destination.num_samples()));
    for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
        auto* out = destination.channel_ptr(channel);
        std::fill_n(out, static_cast<std::size_t>(frame_count), 0.0f);
    }
}

float read_linear(const float* source,
                  std::uint64_t source_frames,
                  double position) noexcept {
    if (source == nullptr ||
        source_frames == 0 ||
        position < 0.0 ||
        !std::isfinite(position)) {
        return 0.0f;
    }

    const auto base = static_cast<std::uint64_t>(std::floor(position));
    if (base >= source_frames) return 0.0f;
    const auto next = std::min<std::uint64_t>(base + 1, source_frames - 1);
    const auto frac = static_cast<float>(position - static_cast<double>(base));
    return source[base] + (source[next] - source[base]) * frac;
}

}  // namespace

SampleVoiceRenderResult SampleVoiceRenderer::render(
    SampleVoiceRenderState& state,
    BufferView<float> destination,
    std::uint64_t frames,
    std::span<const float*> channel_scratch,
    const SampleVoiceRenderOptions& options) noexcept {
    SampleVoiceRenderResult result;
    if (frames == 0 || destination.empty()) return result;

    const auto frame_count = std::min<std::uint64_t>(
        frames,
        static_cast<std::uint64_t>(destination.num_samples()));

    if (!options.accumulate) {
        clear_destination(destination, frame_count);
    }

    if (!state.active) {
        result.silent_frames = frame_count;
        return result;
    }

    if (options.envelope != nullptr && !options.envelope->active()) {
        state.active = false;
        result.finished = true;
        result.silent_frames = frame_count;
        return result;
    }

    if (!state.sample.valid ||
        state.sample.view.num_channels == 0 ||
        state.sample.view.num_frames == 0 ||
        !positive_finite(state.playback_rate) ||
        !std::isfinite(state.gain) ||
        state.position_frames < 0.0 ||
        !std::isfinite(state.position_frames) ||
        channel_scratch.size() < state.sample.view.num_channels ||
        !SamplePool::populate_channel_ptrs(state.sample,
                                           channel_scratch.data(),
                                           channel_scratch.size())) {
        state.active = false;
        result.finished = true;
        result.silent_frames = frame_count;
        return result;
    }

    const auto source_channels =
        static_cast<std::size_t>(state.sample.view.num_channels);
    const auto source_frames = state.sample.view.num_frames;

    for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
        if (state.position_frames >= static_cast<double>(source_frames)) {
            state.active = false;
            result.finished = true;
            result.silent_frames = frame_count - frame;
            break;
        }

        auto envelope_gain = 1.0f;
        if (options.envelope != nullptr) {
            envelope_gain = options.envelope->next_sample();
        }
        const auto gain = state.gain * envelope_gain;

        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            const auto source_channel = std::min(channel, source_channels - 1);
            const auto sample = read_linear(channel_scratch[source_channel],
                                            source_frames,
                                            state.position_frames) * gain;
            destination.channel_ptr(channel)[frame] += sample;
        }

        ++result.rendered_frames;
        state.position_frames += state.playback_rate;

        if (options.envelope != nullptr &&
            !options.envelope->active() &&
            envelope_gain <= 0.0f) {
            state.active = false;
            result.finished = true;
            result.silent_frames = frame_count - frame - 1;
            break;
        }
    }

    if (result.rendered_frames == frame_count &&
        state.position_frames >= static_cast<double>(source_frames)) {
        state.active = false;
        result.finished = true;
    }

    return result;
}

}  // namespace pulp::audio
