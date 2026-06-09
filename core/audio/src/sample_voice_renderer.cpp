#include <pulp/audio/sample_voice_renderer.hpp>

#include <pulp/audio/loop_reader.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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

LoopRegion full_sample_one_shot(const SamplePoolResolution& sample,
                                LoopInterpolationMode interpolation) noexcept {
    LoopRegion region;
    region.start_frame = 0;
    region.end_frame = sample.view.num_frames;
    region.crossfade_frames = 0;
    region.source_sample_rate = sample.view.sample_rate;
    region.playback_mode = LoopPlaybackMode::OneShot;
    region.interpolation = interpolation;
    return region;
}

LoopRegion playback_region_for(const SampleVoiceRenderState& state,
                               LoopInterpolationMode interpolation) noexcept {
    auto region = state.use_playback_region
                      ? state.playback_region
                      : full_sample_one_shot(state.sample, interpolation);
    if (!positive_finite(region.source_sample_rate)) {
        region.source_sample_rate = state.sample.view.sample_rate;
    }
    return region;
}

bool one_shot_position_finished(const LoopRegion& region,
                                double position) noexcept {
    return region.playback_mode == LoopPlaybackMode::OneShot &&
           (position < static_cast<double>(region.start_frame) ||
            position >= static_cast<double>(region.end_frame));
}

double playback_step_for(const LoopRegion& region,
                         double playback_rate) noexcept {
    return region.playback_mode == LoopPlaybackMode::Reverse
               ? -playback_rate
               : playback_rate;
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
        !positive_finite(state.sample.view.sample_rate) ||
        !positive_finite(state.playback_rate) ||
        !std::isfinite(state.gain) ||
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
    if (source_frames > static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max())) {
        state.active = false;
        result.finished = true;
        result.silent_frames = frame_count;
        return result;
    }

    const auto playback_region =
        playback_region_for(state, options.interpolation);
    if (!validate_loop_region(playback_region, source_frames).ok) {
        state.active = false;
        result.finished = true;
        result.silent_frames = frame_count;
        return result;
    }

    BufferView<const float> source_view(
        channel_scratch.data(),
        source_channels,
        static_cast<std::size_t>(source_frames));
    const auto step = playback_step_for(playback_region, state.playback_rate);

    for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
        if (one_shot_position_finished(playback_region, state.position_frames)) {
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
            const auto output_channel =
                channel > std::numeric_limits<std::uint32_t>::max()
                    ? std::numeric_limits<std::uint32_t>::max()
                    : static_cast<std::uint32_t>(channel);
            const auto sample = LoopReader::read_validated(
                                    source_view,
                                    playback_region,
                                    output_channel,
                                    state.position_frames) * gain;
            destination.channel_ptr(channel)[frame] += sample;
        }

        ++result.rendered_frames;
        state.position_frames += step;
        if (playback_region.playback_mode != LoopPlaybackMode::OneShot) {
            state.position_frames =
                LoopReader::normalize_position(playback_region, state.position_frames);
        }

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
        one_shot_position_finished(playback_region, state.position_frames)) {
        state.active = false;
        result.finished = true;
    }

    return result;
}

}  // namespace pulp::audio
