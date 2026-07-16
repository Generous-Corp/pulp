#include "sampler_loop_parity.hpp"

#include <algorithm>
#include <cmath>

namespace pulp::test::sampler_loop_parity {

namespace {

constexpr double kHalfPi = 1.57079632679489661923132169163975144;

bool valid_config(const LoopOracleConfig& config) noexcept {
    const auto& region = config.region;
    if (region.start_frame >= region.end_frame) return false;
    const auto length = region.end_frame - region.start_frame;
    return std::isfinite(region.source_sample_rate) &&
           region.source_sample_rate > 0.0 &&
           region.crossfade_frames <= length / 2 &&
           std::isfinite(config.playback_rate) && config.playback_rate > 0.0;
}

double wrap_position(double position, double start, double length) noexcept {
    double offset = std::fmod(position - start, length);
    if (offset < 0.0) offset += length;
    return start + offset;
}

std::uint64_t wrapped_crossing_count(double position,
                                     double distance,
                                     int direction,
                                     double start,
                                     double length) noexcept {
    const double offset = position - start;
    if (direction > 0) {
        return static_cast<std::uint64_t>(
            std::floor((offset + distance) / length));
    }
    if (distance <= offset) return 0;
    return static_cast<std::uint64_t>(
        std::ceil((distance - offset) / length));
}

struct OracleState {
    double position = 0.0;
    int direction = 1;
    bool active = true;
    bool steady_state = false;
};

std::uint64_t advance_ping_pong(OracleState& state,
                                double distance,
                                double start,
                                double last) noexcept {
    if (last <= start) {
        state.position = start;
        return 0;
    }

    std::uint64_t crossings = 0;
    while (distance > 0.0) {
        const double to_edge = state.direction > 0
                                   ? last - state.position
                                   : state.position - start;
        if (distance <= to_edge) {
            state.position += static_cast<double>(state.direction) * distance;
            break;
        }
        state.position = state.direction > 0 ? last : start;
        distance -= to_edge;
        state.direction = -state.direction;
        ++crossings;
    }
    if (crossings > 0 &&
        (state.position == start || state.position == last)) {
        state.direction = 1;
    }
    return crossings;
}

std::uint64_t advance_steady_loop(OracleState& state,
                                  double distance,
                                  double start,
                                  double length) noexcept {
    const auto crossings = wrapped_crossing_count(
        state.position, distance, state.direction, start, length);
    state.position = wrap_position(
        state.position + static_cast<double>(state.direction) * distance,
        start, length);
    return crossings;
}

std::uint64_t advance_loop(OracleState& state,
                           const audio::LoopRegion& region,
                           double distance) noexcept {
    const double start = static_cast<double>(region.start_frame);
    const double end = static_cast<double>(region.end_frame);
    const double last = end - 1.0;
    const double length = end - start;
    const int loop_direction =
        region.playback_mode == audio::LoopPlaybackMode::Reverse ? -1 : 1;

    if (state.direction == loop_direction) {
        state.steady_state = true;
        return advance_steady_loop(state, distance, start, length);
    }

    const double to_turn = state.direction > 0
                               ? last - state.position
                               : state.position - start;
    if (distance <= to_turn) {
        state.position += static_cast<double>(state.direction) * distance;
        return 0;
    }

    state.position = state.direction > 0 ? last : start;
    distance -= to_turn;
    state.direction = loop_direction;
    state.steady_state = true;
    return 1 + advance_steady_loop(state, distance, start, length);
}

std::uint64_t advance(OracleState& state,
                      const audio::LoopRegion& region,
                      double distance) noexcept {
    const double start = static_cast<double>(region.start_frame);
    const double end = static_cast<double>(region.end_frame);
    const double next = state.position +
                        static_cast<double>(state.direction) * distance;

    if (region.playback_mode == audio::LoopPlaybackMode::OneShot ||
        region.playback_mode == audio::LoopPlaybackMode::ReverseOnce) {
        state.position = next;
        if (state.position < start || state.position >= end) state.active = false;
        return 0;
    }

    if (region.playback_mode == audio::LoopPlaybackMode::PingPong) {
        return advance_ping_pong(state, distance, start, end - 1.0);
    }

    return advance_loop(state, region, distance);
}

}  // namespace

LoopCrossfadePlan make_loop_crossfade_plan(
    const audio::LoopRegion& region,
    double position,
    double playback_step,
    bool steady_state) noexcept {
    LoopCrossfadePlan plan;
    plan.primary_position = position;

    const int loop_direction =
        region.playback_mode == audio::LoopPlaybackMode::Reverse ? -1 : 1;
    const bool loop_wraps =
        region.playback_mode == audio::LoopPlaybackMode::Forward ||
        region.playback_mode == audio::LoopPlaybackMode::Reverse;
    const int direction = playback_step < 0.0 ? -1 : 1;
    if (!loop_wraps || !steady_state || !std::isfinite(playback_step) ||
        playback_step == 0.0 || direction != loop_direction ||
        region.crossfade_frames == 0) {
        return plan;
    }

    const double start = static_cast<double>(region.start_frame);
    const double end = static_cast<double>(region.end_frame);
    const double crossfade = static_cast<double>(region.crossfade_frames);
    if (position < start || position >= end) return plan;
    double progress = 0.0;

    if (direction > 0) {
        const double fade_start = end - crossfade;
        double fade_position = position;
        if (position < fade_start && position + playback_step >= fade_start) {
            fade_position = std::min(position + playback_step, end);
        } else if (position < fade_start || position >= end) {
            return plan;
        }
        progress = (fade_position - fade_start) / crossfade;
        plan.blend_position = start + (fade_position - fade_start);
    } else {
        const double fade_end = start + crossfade;
        double fade_position = position;
        if (position >= fade_end && position + playback_step < fade_end) {
            fade_position = std::max(position + playback_step, start);
        } else if (position < start || position >= fade_end) {
            return plan;
        }
        progress = (fade_end - fade_position) / crossfade;
        plan.blend_position = end - (fade_end - fade_position);
    }

    progress = std::clamp(progress, 0.0, 1.0);
    if (region.crossfade_curve == audio::LoopCrossfadeCurve::EqualPower) {
        plan.primary_gain = std::cos(progress * kHalfPi);
        plan.blend_gain = std::sin(progress * kHalfPi);
    } else {
        plan.primary_gain = 1.0 - progress;
        plan.blend_gain = progress;
    }
    plan.blend = true;
    return plan;
}

std::optional<std::vector<LoopOracleTap>> make_loop_oracle_schedule(
    const LoopOracleConfig& config) {
    if (!valid_config(config)) return std::nullopt;

    const auto& region = config.region;
    const bool reverse_entry =
        region.reverse_entry ||
        region.playback_mode == audio::LoopPlaybackMode::ReverseOnce;
    OracleState state;
    state.position = reverse_entry
                         ? static_cast<double>(region.end_frame - 1)
                         : static_cast<double>(region.start_frame);
    state.direction = reverse_entry ? -1 : 1;
    state.steady_state =
        region.playback_mode == audio::LoopPlaybackMode::Forward
            ? state.direction > 0
            : region.playback_mode == audio::LoopPlaybackMode::Reverse
                  ? state.direction < 0
                  : region.playback_mode == audio::LoopPlaybackMode::PingPong;

    std::vector<LoopOracleTap> schedule;
    schedule.reserve(static_cast<std::size_t>(config.output_frames));
    for (std::uint64_t frame = 0; frame < config.output_frames; ++frame) {
        LoopOracleTap tap;
        tap.output_frame = frame;
        tap.active = state.active;
        tap.direction = state.direction;
        tap.steady_state = state.steady_state;
        if (state.active) {
            const auto crossfade = make_loop_crossfade_plan(
                region,
                state.position,
                static_cast<double>(state.direction) * config.playback_rate,
                state.steady_state);
            tap.primary_position = crossfade.primary_position;
            tap.blend_position = crossfade.blend_position;
            tap.primary_gain = crossfade.primary_gain;
            tap.blend_gain = crossfade.blend_gain;
            tap.blend = crossfade.blend;
            tap.boundary_crossings_after =
                advance(state, region, config.playback_rate);
        }
        schedule.push_back(tap);
    }
    return schedule;
}

}  // namespace pulp::test::sampler_loop_parity
