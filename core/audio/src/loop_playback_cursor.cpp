#include <pulp/audio/loop_playback_cursor.hpp>

#include <pulp/audio/loop_reader.hpp>
#include <pulp/signal/crossfade.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

namespace {

struct BlendGains {
    double dry;
    double wet;
};

BlendGains blend_gains(double position, LoopCrossfadeCurve curve) noexcept {
    const double normalized = std::clamp(position, 0.0, 1.0);
    BlendGains gains{};
    signal::crossfade_gains(
        normalized,
        curve == LoopCrossfadeCurve::EqualPower
            ? signal::CrossfadeGainLaw::EqualPower
            : signal::CrossfadeGainLaw::EqualGain,
        gains.dry,
        gains.wet);
    return gains;
}

}  // namespace

bool LoopPlaybackCursor::set_region(const LoopRegion& region,
                                    std::uint64_t source_frames) noexcept {
    if (!validate_loop_region(region, source_frames).ok) {
        reset();
        return false;
    }
    region_ = region;
    reset();
    return true;
}

void LoopPlaybackCursor::initialize_entry() noexcept {
    const bool enter_reverse =
        region_.reverse_entry ||
        region_.playback_mode == LoopPlaybackMode::ReverseOnce;
    step_direction_ = enter_reverse ? -1 : 1;
    pingpong_direction_ = step_direction_;
    position_ = enter_reverse ? static_cast<double>(region_.end_frame - 1)
                              : static_cast<double>(region_.start_frame);
}

void LoopPlaybackCursor::reset() noexcept {
    initialize_entry();
    active_ = false;
}

void LoopPlaybackCursor::start() noexcept {
    initialize_entry();
    active_ = true;
}

void LoopPlaybackCursor::set_playback_rate(double rate) noexcept {
    if (std::isfinite(rate) && rate != 0.0) playback_rate_ = rate;
}

double LoopPlaybackCursor::step() const noexcept {
    const int direction = region_.playback_mode == LoopPlaybackMode::PingPong
        ? pingpong_direction_
        : step_direction_;
    return playback_rate_ * static_cast<double>(direction);
}

LoopFrameReadPlan LoopPlaybackCursor::frame_read_plan() const noexcept {
    LoopFrameReadPlan plan;
    const auto current_step = step();
    const int loop_direction =
        region_.playback_mode == LoopPlaybackMode::Reverse ? -1 : 1;
    if (region_.playback_mode == LoopPlaybackMode::OneShot ||
        region_.playback_mode == LoopPlaybackMode::ReverseOnce ||
        region_.playback_mode == LoopPlaybackMode::PingPong ||
        region_.crossfade_frames == 0 ||
        step_direction_ != loop_direction) {
        plan.read_position = position_;
        return plan;
    }

    const auto crossfade = static_cast<double>(region_.crossfade_frames);
    const auto start = static_cast<double>(region_.start_frame);
    const auto end = static_cast<double>(region_.end_frame);
    const auto normalized = LoopReader::normalize_position(region_, position_);

    auto make_blend = [&](double blend_position, double wrapped_position) {
        const auto gains = blend_gains(blend_position, region_.crossfade_curve);
        plan.blend = true;
        plan.wrapped = true;
        plan.read_position = normalized;
        plan.blend_position = wrapped_position;
        plan.primary_gain = gains.dry;
        plan.blend_gain = gains.wet;
    };

    if (current_step >= 0.0 && normalized >= end - crossfade) {
        make_blend((normalized - (end - crossfade)) / crossfade,
                   start + (normalized - (end - crossfade)));
        return plan;
    }

    if (current_step > 0.0 && normalized < end - crossfade &&
        normalized + current_step >= end - crossfade) {
        const auto probe = std::min(normalized + current_step, end);
        make_blend((probe - (end - crossfade)) / crossfade,
                   start + (probe - (end - crossfade)));
        return plan;
    }

    if (current_step < 0.0 && normalized < start + crossfade) {
        make_blend(((start + crossfade) - normalized) / crossfade,
                   end - ((start + crossfade) - normalized));
        return plan;
    }

    if (current_step < 0.0 && normalized >= start + crossfade &&
        normalized + current_step < start + crossfade) {
        const auto probe = std::max(normalized + current_step, start);
        make_blend(((start + crossfade) - probe) / crossfade,
                   end - ((start + crossfade) - probe));
        return plan;
    }

    plan.read_position = normalized;
    return plan;
}

LoopPlaybackAdvanceResult LoopPlaybackCursor::advance() noexcept {
    LoopPlaybackAdvanceResult result{.active = active_};
    if (!active_) return result;

    const auto current_step = step();
    const auto next = position_ + current_step;
    if (region_.playback_mode == LoopPlaybackMode::OneShot ||
        region_.playback_mode == LoopPlaybackMode::ReverseOnce) {
        position_ = next;
        if (position_ < static_cast<double>(region_.start_frame) ||
            position_ >= static_cast<double>(region_.end_frame)) {
            active_ = false;
        }
        result.active = active_;
        return result;
    }

    const auto start = static_cast<double>(region_.start_frame);
    const auto end = static_cast<double>(region_.end_frame);
    const auto last = end - 1.0;

    if (region_.playback_mode == LoopPlaybackMode::PingPong) {
        if (last <= start) {
            position_ = start;
            return result;
        }
        if (next >= start && next <= last) {
            position_ = next;
            return result;
        }

        const auto span = last - start;
        const auto period = span * 2.0;
        const auto distance = std::abs(current_step);
        const bool moving_forward = current_step > 0.0;
        const auto phase = moving_forward
            ? position_ - start
            : period - (position_ - start);
        const auto residual_distance = std::fmod(distance, period);
        auto folded_phase = std::fmod(phase + residual_distance, period);
        if (folded_phase < 0.0) folded_phase += period;

        int travel_direction = 0;
        if (folded_phase == 0.0) {
            position_ = start;
            travel_direction = 1;
        } else if (folded_phase < span) {
            position_ = start + folded_phase;
            travel_direction = 1;
        } else if (folded_phase == span) {
            position_ = last;
            travel_direction = 1;
        } else {
            position_ = last - (folded_phase - span);
            travel_direction = -1;
        }
        const int rate_sign = playback_rate_ > 0.0 ? 1 : -1;
        pingpong_direction_ = travel_direction * rate_sign;
        result.wrapped = true;
        return result;
    }

    const int loop_direction =
        region_.playback_mode == LoopPlaybackMode::Reverse ? -1 : 1;
    if (current_step > 0.0 && next >= end) {
        result.wrapped = true;
        if (loop_direction > 0) {
            position_ = LoopReader::normalize_position(region_, next);
            return result;
        }
        step_direction_ = -1;
        const auto reflected = last - (next - last);
        position_ = reflected >= start && reflected < end
            ? reflected
            : LoopReader::normalize_position(region_, reflected);
        return result;
    }
    if (current_step < 0.0 && next < start) {
        result.wrapped = true;
        if (loop_direction < 0) {
            position_ = LoopReader::normalize_position(region_, next);
            return result;
        }
        step_direction_ = 1;
        const auto reflected = start + (start - next);
        position_ = reflected >= start && reflected < end
            ? reflected
            : LoopReader::normalize_position(region_, reflected);
        return result;
    }
    position_ = next;
    return result;
}

}  // namespace pulp::audio
