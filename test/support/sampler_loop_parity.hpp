#pragma once

#include <pulp/audio/loop_types.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::test::sampler_loop_parity {

struct LoopOracleConfig {
    audio::LoopRegion region;
    double playback_rate = 1.0;
    std::uint64_t output_frames = 0;
};

struct LoopCrossfadePlan {
    double primary_position = 0.0;
    double blend_position = 0.0;
    double primary_gain = 1.0;
    double blend_gain = 0.0;
    bool blend = false;
};

struct LoopOracleTap {
    std::uint64_t output_frame = 0;
    double primary_position = 0.0;
    double blend_position = 0.0;
    double primary_gain = 1.0;
    double blend_gain = 0.0;
    std::uint64_t boundary_crossings_after = 0;
    int direction = 1;
    bool active = false;
    bool steady_state = false;
    bool blend = false;
};

LoopCrossfadePlan make_loop_crossfade_plan(
    const audio::LoopRegion& region,
    double position,
    double playback_step,
    bool steady_state) noexcept;

std::optional<std::vector<LoopOracleTap>> make_loop_oracle_schedule(
    const LoopOracleConfig& config);

}  // namespace pulp::test::sampler_loop_parity
