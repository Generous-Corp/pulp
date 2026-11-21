#pragma once

#include <cstddef>
#include <cstdint>

namespace pulp::vellum_audio {

inline constexpr std::size_t kMaxMeterChannels = 32;
inline constexpr std::size_t kMaxScopeSamples = 4096;

struct AudioFrameInfo {
    float viewport_min_hz = 20.0f;
    float viewport_max_hz = 20000.0f;
    float floor_db = -120.0f;
    double sample_rate = 48000.0;
    std::uint64_t epoch = 0;
    std::uint32_t sequence = 0;
    std::uint32_t dropped_frames = 0;
};

} // namespace pulp::vellum_audio
