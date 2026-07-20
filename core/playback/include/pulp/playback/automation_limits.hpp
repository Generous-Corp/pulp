#pragma once

#include <cstdint>

namespace pulp::playback {

/// Preparation and callback-work ceilings shared by document compilation and
/// track automation rendering. Applications may lower these values, but hard
/// maxima keep caller-supplied configurations within portable memory bounds.
struct AutomationPlaybackLimits {
    static constexpr std::uint32_t kMaximumDevicePlacementsPerTrack = 1'024;
    static constexpr std::uint32_t kMaximumLanesPerTrack = 1'024;
    static constexpr std::uint32_t kMaximumPointsPerLane = 65'536;
    static constexpr std::uint32_t kMaximumPointsPerTrack = 262'144;
    static constexpr std::uint32_t kMaximumRenderCandidatesPerBlock = 65'536;
    static constexpr std::uint32_t kMaximumEventsPerDevicePerBlock = 1'024;

    std::uint32_t max_device_placements_per_track = 64;
    std::uint32_t max_lanes_per_track = 256;
    std::uint32_t max_points_per_lane = 16'384;
    std::uint32_t max_points_per_track = 65'536;
    std::uint32_t max_render_candidates_per_block = 8'192;
    std::uint32_t max_events_per_device_per_block = 1'024;

    static constexpr AutomationPlaybackLimits web_defaults() noexcept {
        return {
            .max_device_placements_per_track = 32,
            .max_lanes_per_track = 128,
            .max_points_per_lane = 8'192,
            .max_points_per_track = 32'768,
            .max_render_candidates_per_block = 4'096,
            .max_events_per_device_per_block = 1'024,
        };
    }

    static constexpr AutomationPlaybackLimits platform_defaults() noexcept {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
        return web_defaults();
#else
        return {};
#endif
    }

    constexpr bool valid() const noexcept {
        return max_device_placements_per_track != 0 && max_lanes_per_track != 0 &&
               max_points_per_lane != 0 && max_points_per_track >= max_points_per_lane &&
               max_render_candidates_per_block != 0 && max_events_per_device_per_block != 0 &&
               max_device_placements_per_track <= kMaximumDevicePlacementsPerTrack &&
               max_lanes_per_track <= kMaximumLanesPerTrack &&
               max_points_per_lane <= kMaximumPointsPerLane &&
               max_points_per_track <= kMaximumPointsPerTrack &&
               max_render_candidates_per_block <= kMaximumRenderCandidatesPerBlock &&
               max_events_per_device_per_block <= kMaximumEventsPerDevicePerBlock;
    }

    constexpr bool operator==(const AutomationPlaybackLimits&) const = default;
};

} // namespace pulp::playback
