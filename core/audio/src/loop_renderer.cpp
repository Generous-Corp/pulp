#include <pulp/audio/loop_renderer.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

bool LoopRenderer::set_region(const LoopRegion& region,
                              std::uint64_t source_frames) noexcept {
    if (!cursor_.set_region(region, source_frames)) {
        reset();
        return false;
    }
    interpolation_ = {
        .policy = sample_interpolation_policy(region.interpolation)};
    reset();
    return true;
}

void LoopRenderer::reset() noexcept {
    cursor_.reset();
    start_fade_position_ = 0;
    stop_fade_position_ = 0;
    stopping_ = false;
}

void LoopRenderer::start() noexcept {
    cursor_.start();
    start_fade_position_ = 0;
    stop_fade_position_ = 0;
    stopping_ = false;
}

void LoopRenderer::stop() noexcept {
    if (!cursor_.active()) return;
    if (stop_fade_frames_ == 0) {
        cursor_.stop();
        stopping_ = false;
        return;
    }
    stopping_ = true;
    stop_fade_position_ = 0;
}

void LoopRenderer::set_playback_rate(double rate) noexcept {
    cursor_.set_playback_rate(rate);
}

bool LoopRenderer::set_interpolation_policy(
    SampleInterpolationPolicy policy) noexcept {
    return set_interpolation({.policy = policy});
}

bool LoopRenderer::set_interpolation(
    const PreparedSampleInterpolation& interpolation) noexcept {
    if (!interpolation.valid()) return false;
    interpolation_ = interpolation;
    return true;
}

float LoopRenderer::fade_gain() noexcept {
    double gain = 1.0;
    if (start_fade_frames_ > 1 && start_fade_position_ < start_fade_frames_) {
        gain *= static_cast<double>(start_fade_position_) /
                static_cast<double>(start_fade_frames_ - 1);
        ++start_fade_position_;
    }

    if (stopping_) {
        if (stop_fade_frames_ <= 1) {
            cursor_.stop();
            stopping_ = false;
            return 0.0f;
        }
        gain *= 1.0 - static_cast<double>(stop_fade_position_) /
                        static_cast<double>(stop_fade_frames_ - 1);
        ++stop_fade_position_;
        if (stop_fade_position_ >= stop_fade_frames_) {
            cursor_.stop();
            stopping_ = false;
        }
    }
    return static_cast<float>(std::clamp(gain, 0.0, 1.0));
}

float LoopRenderer::apply_crossfade_plan(BufferView<const float> source,
                                         std::uint32_t output_channel,
                                         const LoopFrameReadPlan& plan) const noexcept {
    const auto& region = cursor_.region();
    if (!plan.blend) {
        return LoopReader::read_validated(source, region, output_channel,
                                          plan.read_position, interpolation_);
    }
    const auto primary = static_cast<double>(
        LoopReader::read_validated(source, region, output_channel,
                                   plan.read_position, interpolation_));
    const auto blend = static_cast<double>(
        LoopReader::read_validated(source, region, output_channel,
                                   plan.blend_position, interpolation_));
    return static_cast<float>(primary * plan.primary_gain +
                              blend * plan.blend_gain);
}

LoopRenderResult LoopRenderer::render(BufferView<const float> source,
                                      BufferView<float> destination,
                                      std::uint64_t frames) noexcept {
    LoopRenderResult result;
    if (destination.num_channels() == 0 || destination.num_samples() == 0 ||
        frames == 0) {
        result.active = cursor_.active();
        return result;
    }
    const auto output_frames =
        std::min(frames, static_cast<std::uint64_t>(destination.num_samples()));
    const auto valid_source =
        source.num_channels() > 0 &&
        validate_loop_region(cursor_.region(),
                             static_cast<std::uint64_t>(source.num_samples())).ok;

    for (std::uint64_t i = 0; i < output_frames; ++i) {
        const bool should_advance = cursor_.active() && valid_source;
        const auto gain = should_advance ? fade_gain() : 0.0f;
        bool sample_wrapped = false;
        LoopFrameReadPlan plan;
        if (gain != 0.0f) {
            plan = cursor_.frame_read_plan();
            sample_wrapped = plan.wrapped;
        }
        for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
            float* channel = destination.channel_ptr(ch);
            const auto sample = gain == 0.0f
                ? 0.0f
                : apply_crossfade_plan(source,
                                       static_cast<std::uint32_t>(ch),
                                       plan) * gain;
            channel[i] = sample;
            if (i > 0) {
                result.max_sample_delta =
                    std::max(result.max_sample_delta,
                             std::abs(sample - channel[i - 1]));
            }
        }

        if (gain == 0.0f) ++result.silent_frames;

        if (should_advance) {
            ++result.source_backed_frames;
            const auto advanced = cursor_.advance();
            sample_wrapped = sample_wrapped || advanced.wrapped;
        }
        result.wrapped = result.wrapped || sample_wrapped;
        ++result.rendered_frames;
    }
    result.active = cursor_.active();
    return result;
}

}  // namespace pulp::audio
