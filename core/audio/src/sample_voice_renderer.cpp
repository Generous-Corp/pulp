#include <pulp/audio/sample_voice_renderer.hpp>

#include <pulp/audio/loop_reader.hpp>
#include <pulp/signal/interpolator.hpp>

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
                         double playback_rate,
                         double host_sample_rate) noexcept {
    const double source_sample_rate = positive_finite(region.source_sample_rate)
                                          ? region.source_sample_rate
                                          : host_sample_rate;
    const double host_rate = positive_finite(host_sample_rate)
                                 ? host_sample_rate
                                 : source_sample_rate;
    const double rate_ratio =
        positive_finite(source_sample_rate) && positive_finite(host_rate)
            ? source_sample_rate / host_rate
            : 1.0;
    const double step = playback_rate * rate_ratio;
    return region.playback_mode == LoopPlaybackMode::Reverse ? -step : step;
}

std::uint32_t source_channel_for(std::uint32_t source_channels,
                                 std::size_t output_channel) noexcept {
    if (source_channels == 0) return 0;
    if (output_channel < source_channels) return static_cast<std::uint32_t>(output_channel);
    return source_channels == 1 ? 0 : source_channels;
}

std::uint64_t wrap_index(const LoopRegion& region, long long frame) noexcept {
    const auto length = static_cast<long long>(region.end_frame - region.start_frame);
    if (length <= 0) return region.start_frame;
    auto relative = frame - static_cast<long long>(region.start_frame);
    relative %= length;
    if (relative < 0) relative += length;
    return region.start_frame + static_cast<std::uint64_t>(relative);
}

std::uint64_t sample_index_for(const LoopRegion& region,
                               std::uint64_t source_frames,
                               long long frame) noexcept {
    if (source_frames == 0) return 0;
    if (region.playback_mode == LoopPlaybackMode::OneShot) {
        const auto source_last = static_cast<long long>(source_frames - 1);
        const auto lo = static_cast<long long>(region.start_frame);
        const auto hi = std::min(static_cast<long long>(region.end_frame - 1), source_last);
        return static_cast<std::uint64_t>(std::clamp(frame, lo, hi));
    }
    return wrap_index(region, frame);
}

float sample_at_channel(const float* source,
                        std::uint64_t source_frames,
                        const LoopRegion& region,
                        long long frame) noexcept {
    if (source == nullptr || source_frames == 0) return 0.0f;
    const auto index = sample_index_for(region, source_frames, frame);
    return index < source_frames ? source[index] : 0.0f;
}

float read_channel_validated(const float* source,
                             std::uint64_t source_frames,
                             const LoopRegion& region,
                             double position) noexcept {
    if (source == nullptr) return 0.0f;
    if (region.playback_mode == LoopPlaybackMode::OneShot &&
        (position < static_cast<double>(region.start_frame) ||
         position >= static_cast<double>(region.end_frame))) {
        return 0.0f;
    }

    const auto normalized = LoopReader::normalize_position(region, position);
    const auto base = static_cast<long long>(std::floor(normalized));
    const auto frac = static_cast<float>(normalized - static_cast<double>(base));

    switch (region.interpolation) {
        case LoopInterpolationMode::None:
            return sample_at_channel(source, source_frames, region, base);
        case LoopInterpolationMode::Linear: {
            const auto y0 = sample_at_channel(source, source_frames, region, base);
            const auto y1 = sample_at_channel(source, source_frames, region, base + 1);
            return pulp::signal::Interpolator::linear(frac, y0, y1);
        }
        case LoopInterpolationMode::Cubic: {
            const auto ym1 = sample_at_channel(source, source_frames, region, base - 1);
            const auto y0 = sample_at_channel(source, source_frames, region, base);
            const auto y1 = sample_at_channel(source, source_frames, region, base + 1);
            const auto y2 = sample_at_channel(source, source_frames, region, base + 2);
            return pulp::signal::Interpolator::hermite(frac, ym1, y0, y1, y2);
        }
    }
    return 0.0f;
}

float fade_out_gain(std::uint32_t position,
                    std::uint32_t frames) noexcept {
    if (frames == 0) return 1.0f;
    const float t = std::min(1.0f,
                             static_cast<float>(position + 1) /
                                 static_cast<float>(frames));
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 1.0f - smooth;
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

    const auto step = playback_step_for(playback_region,
                                        state.playback_rate,
                                        state.host_sample_rate);
    const bool fade_active = state.fade_out_frames > 0;

    if (options.envelope == nullptr && !fade_active) {
        const auto dest_channels = destination.num_channels();
        for (std::size_t channel = 0; channel < dest_channels; ++channel) {
            const auto source_channel = source_channel_for(
                static_cast<std::uint32_t>(source_channels), channel);
            if (source_channel >= source_channels) continue;
            const float* source = channel_scratch[source_channel];
            float* out = destination.channel_ptr(channel);
            double position = state.position_frames;
            for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
                if (one_shot_position_finished(playback_region, position)) break;
                out[frame] += read_channel_validated(source,
                                                     source_frames,
                                                     playback_region,
                                                     position) * state.gain;
                position += step;
                if (playback_region.playback_mode != LoopPlaybackMode::OneShot) {
                    position = LoopReader::normalize_position(playback_region, position);
                }
            }
        }

        for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
            if (one_shot_position_finished(playback_region, state.position_frames)) {
                state.active = false;
                result.finished = true;
                result.silent_frames = frame_count - frame;
                break;
            }
            ++result.rendered_frames;
            state.position_frames += step;
            if (playback_region.playback_mode != LoopPlaybackMode::OneShot) {
                state.position_frames =
                    LoopReader::normalize_position(playback_region, state.position_frames);
            }
        }

        if (result.rendered_frames == frame_count &&
            one_shot_position_finished(playback_region, state.position_frames)) {
            state.active = false;
            result.finished = true;
        }

        return result;
    }

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
        const float fade_gain = fade_active
                                    ? fade_out_gain(state.fade_out_position,
                                                    state.fade_out_frames)
                                    : 1.0f;
        const auto gain = state.gain * envelope_gain * fade_gain;

        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            const auto source_channel =
                source_channel_for(static_cast<std::uint32_t>(source_channels), channel);
            const float* source =
                source_channel < source_channels ? channel_scratch[source_channel] : nullptr;
            const auto sample = read_channel_validated(source,
                                                       source_frames,
                                                       playback_region,
                                                       state.position_frames) * gain;
            destination.channel_ptr(channel)[frame] += sample;
        }

        ++result.rendered_frames;
        state.position_frames += step;
        if (fade_active) {
            ++state.fade_out_position;
        }
        if (playback_region.playback_mode != LoopPlaybackMode::OneShot) {
            state.position_frames =
                LoopReader::normalize_position(playback_region, state.position_frames);
        }

        if (fade_active && state.fade_out_position >= state.fade_out_frames) {
            state.active = false;
            state.fade_out_frames = 0;
            state.fade_out_position = 0;
            result.finished = true;
            result.silent_frames = frame_count - frame - 1;
            break;
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

void SampleVoiceRenderer::begin_fade_out(SampleVoiceRenderState& state,
                                         std::uint32_t fade_frames) noexcept {
    if (!state.active || fade_frames == 0) {
        state.active = false;
        state.fade_out_frames = 0;
        state.fade_out_position = 0;
        return;
    }
    state.fade_out_frames = fade_frames;
    state.fade_out_position = 0;
}

}  // namespace pulp::audio
