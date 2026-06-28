#pragma once

#include <cstdint>

namespace pulp::audio {

enum class LoopPlaybackMode : std::uint8_t {
    OneShot,
    Forward,
    Reverse,
    PingPong,      // forward then backward, reflecting at the loop boundaries
    ReverseOnce,   // play once from end to start, then stop (no loop)
};

enum class LoopCrossfadeCurve : std::uint8_t {
    Linear,
    EqualPower,
};

enum class LoopInterpolationMode : std::uint8_t {
    None,
    Linear,
    Cubic,
};

enum class LoopSnapPolicy : std::uint8_t {
    None,
    NearestZeroCrossing,
    SlopeMatchedZeroCrossing,
    ValueDirection,
};

enum class LoopValidationStatus : std::uint8_t {
    Ok,
    EmptySource,
    InvalidRange,
    TooShort,
    CrossfadeTooLong,
    InvalidSampleRate,
};

struct LoopRegion {
    std::uint64_t start_frame = 0;
    std::uint64_t end_frame = 0;  // exclusive
    std::uint64_t crossfade_frames = 0;
    double source_sample_rate = 0.0;
    LoopPlaybackMode playback_mode = LoopPlaybackMode::Forward;
    LoopCrossfadeCurve crossfade_curve = LoopCrossfadeCurve::Linear;
    LoopInterpolationMode interpolation = LoopInterpolationMode::Linear;
    LoopSnapPolicy snap_policy = LoopSnapPolicy::ValueDirection;
};

struct LoopValidationResult {
    bool ok = false;
    LoopValidationStatus status = LoopValidationStatus::InvalidRange;
};

LoopValidationResult validate_loop_region(const LoopRegion& region,
                                          std::uint64_t source_frames,
                                          std::uint64_t min_loop_frames = 1) noexcept;

}  // namespace pulp::audio
