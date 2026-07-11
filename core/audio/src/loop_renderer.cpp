#include <pulp/audio/loop_renderer.hpp>

#include <pulp/signal/crossfade.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

namespace {

struct BlendGains {
    double dry;
    double wet;
};

// Old->new blend gains for a loop-wrap crossfade position `t`, via the shared
// SIGNAL crossfade law so the wrap fades by the same math as the live-swap and
// convolver crossfades. The loop wrap uses the RAW ramp (no smoothstep shaping),
// so `t` is only clamped before the gain split. Computed ONCE per frame so the
// equal-power cos/sin is reused across every channel. Bit-identical to the
// previous inline `t*0.5*π` cos/sin (`t·0.5` is exact, so the shared
// full-precision π/2 rounds the product identically).
BlendGains blend_gains(double t, LoopCrossfadeCurve curve) noexcept {
    const double u = std::clamp(t, 0.0, 1.0);
    BlendGains g{};
    signal::crossfade_gains(u,
                            curve == LoopCrossfadeCurve::EqualPower
                                ? signal::CrossfadeGainLaw::EqualPower
                                : signal::CrossfadeGainLaw::EqualGain,
                            g.dry, g.wet);
    return g;
}

}  // namespace

bool LoopRenderer::set_region(const LoopRegion& region,
                              std::uint64_t source_frames) noexcept {
    if (!validate_loop_region(region, source_frames).ok) {
        reset();
        return false;
    }
    region_ = region;
    source_frames_ = source_frames;
    reset();
    return true;
}

// The first-pass entry: reverse_entry (or ReverseOnce, which always enters
// backward) starts at the top edge travelling backward; otherwise at the bottom
// edge travelling forward. The loop's steady direction (Forward/Reverse) takes
// over later in advance_position().
void LoopRenderer::init_entry() noexcept {
    const bool enter_reverse =
        region_.reverse_entry ||
        region_.playback_mode == LoopPlaybackMode::ReverseOnce;
    step_dir_ = enter_reverse ? -1 : 1;
    pingpong_dir_ = step_dir_;  // PingPong starts bouncing in the entry direction
    position_ = enter_reverse ? static_cast<double>(region_.end_frame - 1)
                              : static_cast<double>(region_.start_frame);
    start_fade_position_ = 0;
    stop_fade_position_ = 0;
}

void LoopRenderer::reset() noexcept {
    init_entry();
    active_ = false;
    stopping_ = false;
}

void LoopRenderer::start() noexcept {
    init_entry();
    active_ = true;
    stopping_ = false;
}

void LoopRenderer::stop() noexcept {
    if (!active_) return;
    if (stop_fade_frames_ == 0) {
        active_ = false;
        stopping_ = false;
        return;
    }
    stopping_ = true;
    stop_fade_position_ = 0;
}

void LoopRenderer::set_playback_rate(double rate) noexcept {
    if (std::isfinite(rate) && rate != 0.0) playback_rate_ = rate;
}

double LoopRenderer::effective_step() const noexcept {
    // PingPong tracks its own bounce direction; every other mode follows
    // step_dir_ (the entry direction, then the loop's steady direction after the
    // first pass). playback_rate_ is a magnitude; the direction is the sign.
    const int dir = (region_.playback_mode == LoopPlaybackMode::PingPong)
                        ? pingpong_dir_
                        : step_dir_;
    return playback_rate_ * static_cast<double>(dir);
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
            active_ = false;
            stopping_ = false;
            return 0.0f;
        }
        gain *= 1.0 - static_cast<double>(stop_fade_position_) /
                        static_cast<double>(stop_fade_frames_ - 1);
        ++stop_fade_position_;
        if (stop_fade_position_ >= stop_fade_frames_) {
            active_ = false;
            stopping_ = false;
        }
    }
    return static_cast<float>(std::clamp(gain, 0.0, 1.0));
}

// Decide the wrap-crossfade for one frame (channel-independent). Computes the
// blend gains (equal-power cos/sin) once here; apply_crossfade_plan() then reuses
// them for every channel. Extracted verbatim from the old per-channel
// sample_with_crossfade — same branches, same positions, same t — so output is
// bit-identical, only with the transcendentals hoisted out of the channel loop.
//
// The wrap-crossfade is only for the STEADY loop wrap (jump start<->end in the
// loop's direction). Skip it for OneShot/ReverseOnce/PingPong (PingPong
// reflects, so it is already continuous), when disabled, and during a
// MISMATCHED first pass (step_dir_ != the loop's steady dir) — there the first
// pass turns around by reflection, not a wrap, so a crossfade toward the wrap
// target would be wrong.
LoopRenderer::CrossfadePlan LoopRenderer::compute_crossfade_plan(
    double position, double step) const noexcept {
    CrossfadePlan plan;

    const int loop_dir = (region_.playback_mode == LoopPlaybackMode::Reverse) ? -1 : 1;
    if (region_.playback_mode == LoopPlaybackMode::OneShot ||
        region_.playback_mode == LoopPlaybackMode::ReverseOnce ||
        region_.playback_mode == LoopPlaybackMode::PingPong ||
        region_.crossfade_frames == 0 ||
        step_dir_ != loop_dir) {
        plan.read_pos = position;
        return plan;
    }

    const auto crossfade = static_cast<double>(region_.crossfade_frames);
    const auto start = static_cast<double>(region_.start_frame);
    const auto end = static_cast<double>(region_.end_frame);
    const auto normalized = LoopReader::normalize_position(region_, position);

    auto make_blend = [&](double t, double wrapped_position) {
        const auto gains = blend_gains(t, region_.crossfade_curve);
        plan.blend = true;
        plan.wrapped = true;
        plan.read_pos = normalized;
        plan.blend_pos = wrapped_position;
        plan.primary_gain = gains.dry;
        plan.blend_gain = gains.wet;
    };

    if (step >= 0.0 && normalized >= end - crossfade) {
        make_blend((normalized - (end - crossfade)) / crossfade,
                   start + (normalized - (end - crossfade)));
        return plan;
    }

    if (step > 0.0 && normalized < end - crossfade &&
        normalized + step >= end - crossfade) {
        const auto probe = std::min(normalized + step, end);
        make_blend((probe - (end - crossfade)) / crossfade,
                   start + (probe - (end - crossfade)));
        return plan;
    }

    if (step < 0.0 && normalized < start + crossfade) {
        make_blend(((start + crossfade) - normalized) / crossfade,
                   end - ((start + crossfade) - normalized));
        return plan;
    }

    if (step < 0.0 && normalized >= start + crossfade &&
        normalized + step < start + crossfade) {
        const auto probe = std::max(normalized + step, start);
        make_blend(((start + crossfade) - probe) / crossfade,
                   end - ((start + crossfade) - probe));
        return plan;
    }

    plan.read_pos = normalized;
    return plan;
}

float LoopRenderer::apply_crossfade_plan(BufferView<const float> source,
                                         std::uint32_t output_channel,
                                         const CrossfadePlan& plan) const noexcept {
    if (!plan.blend) {
        return LoopReader::read_validated(source, region_, output_channel,
                                          plan.read_pos);
    }
    const auto a = static_cast<double>(
        LoopReader::read_validated(source, region_, output_channel, plan.read_pos));
    const auto b = static_cast<double>(
        LoopReader::read_validated(source, region_, output_channel, plan.blend_pos));
    return static_cast<float>(a * plan.primary_gain + b * plan.blend_gain);
}

double LoopRenderer::advance_position(double position, double step, bool& wrapped) noexcept {
    const auto next = position + step;
    // OneShot and ReverseOnce play once and stop — no wrap.
    if (region_.playback_mode == LoopPlaybackMode::OneShot ||
        region_.playback_mode == LoopPlaybackMode::ReverseOnce)
        return next;

    if (region_.playback_mode == LoopPlaybackMode::PingPong) {
        // Reflect at the loop boundaries and flip direction. Playable range is
        // [start, last] with last = end - 1 (end is exclusive). Reflect any
        // overshoot back into range so the position stays valid every frame.
        const auto start = static_cast<double>(region_.start_frame);
        const auto last = static_cast<double>(region_.end_frame) - 1.0;
        if (last <= start) return start;  // degenerate 1-frame region
        double reflected = next;
        if (reflected > last) {
            reflected = last - (reflected - last);  // bounce off the top
            pingpong_dir_ = -1;
            wrapped = true;
        } else if (reflected < start) {
            reflected = start + (start - reflected);  // bounce off the bottom
            pingpong_dir_ = 1;
            wrapped = true;
        }
        // A step larger than the loop could overshoot the far side too; clamp.
        return std::clamp(reflected, start, last);
    }

    // Forward / Reverse loops — two-phase. Travel in the entry direction
    // (step_dir_) until the first pass reaches the far edge, then switch to the
    // loop's STEADY direction (Forward => +1, Reverse => -1). When entry and
    // steady agree it is a plain seamless wrap; when they differ the first pass
    // turns around ONCE (reflected, click-free) and then loops in the steady dir.
    const auto start = static_cast<double>(region_.start_frame);
    const auto end = static_cast<double>(region_.end_frame);
    const auto last = end - 1.0;
    const int loop_dir = (region_.playback_mode == LoopPlaybackMode::Reverse) ? -1 : 1;

    if (step > 0.0 && next >= end) {
        wrapped = true;
        if (loop_dir > 0)  // forward first pass IS the forward loop → seamless wrap
            return LoopReader::normalize_position(region_, next);
        step_dir_ = -1;    // forward first pass done → loop backward from the top
        if (last <= start) return start;
        return std::clamp(last - (next - last), start, last);  // reflect at the top
    }
    if (step < 0.0 && next < start) {
        wrapped = true;
        if (loop_dir < 0)  // backward first pass IS the backward loop → seamless wrap
            return LoopReader::normalize_position(region_, next);
        step_dir_ = 1;     // backward first pass done → loop forward from the bottom
        if (last <= start) return start;
        return std::clamp(start + (start - next), start, last);  // reflect at the bottom
    }
    return next;  // still within range — keep advancing in the current direction
}

LoopRenderResult LoopRenderer::render(BufferView<const float> source,
                                      BufferView<float> destination,
                                      std::uint64_t frames) noexcept {
    LoopRenderResult result;
    if (destination.num_channels() == 0 || destination.num_samples() == 0 || frames == 0) {
        result.active = active_;
        return result;
    }
    const auto output_frames =
        std::min(frames, static_cast<std::uint64_t>(destination.num_samples()));
    const auto valid_source =
        source.num_channels() > 0 &&
        validate_loop_region(region_, static_cast<std::uint64_t>(source.num_samples())).ok;
    auto step = effective_step();  // PingPong re-reads this after each reflection (dir flips)

    for (std::uint64_t i = 0; i < output_frames; ++i) {
        const bool should_advance = active_ && valid_source;
        const auto gain = should_advance ? fade_gain() : 0.0f;
        bool sample_wrapped = false;
        // Compute the wrap-crossfade plan ONCE per frame (channel-independent),
        // so the equal-power cos/sin runs once instead of once per channel.
        CrossfadePlan plan;
        if (gain != 0.0f) {
            plan = compute_crossfade_plan(position_, step);
            sample_wrapped = plan.wrapped;
        }
        for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
            float* channel = destination.channel_ptr(ch);
            const auto sample =
                gain == 0.0f
                    ? 0.0f
                    : apply_crossfade_plan(source,
                                           static_cast<std::uint32_t>(ch),
                                           plan) * gain;
            channel[i] = sample;
            if (i > 0) {
                result.max_sample_delta =
                    std::max(result.max_sample_delta, std::abs(sample - channel[i - 1]));
            }
        }

        if (gain == 0.0f) {
            ++result.silent_frames;
        }

        if (should_advance) {
            position_ = advance_position(position_, step, sample_wrapped);
            // Re-read the step: PingPong flips its bounce direction at each edge,
            // and a Forward/Reverse loop flips step_dir_ when the first pass turns
            // around into the steady loop direction. Cheap; keeps the next frame's
            // travel direction correct.
            step = effective_step();
            if ((region_.playback_mode == LoopPlaybackMode::OneShot ||
                 region_.playback_mode == LoopPlaybackMode::ReverseOnce) &&
                (position_ < static_cast<double>(region_.start_frame) ||
                 position_ >= static_cast<double>(region_.end_frame))) {
                active_ = false;
            }
        }
        result.wrapped = result.wrapped || sample_wrapped;
        ++result.rendered_frames;
    }
    result.active = active_;
    return result;
}

}  // namespace pulp::audio
