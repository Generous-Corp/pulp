#pragma once

#include <compare>
#include <cstdint>

namespace pulp::playback {

/// Shared compile and RT ceilings. Kept separate from the renderer API so the
/// structural PlaybackProgram header does not pull buffer or decoder surfaces.
struct AudioRendererLimits {
    std::uint32_t max_channels = 64;
    std::uint32_t max_block_frames = 1u << 20u;
    std::uint64_t max_asset_frames = 100'000'000u;
    std::uint64_t max_tracks = 4'096u;
    std::uint64_t max_clips = 1'000'000u;
    constexpr auto operator<=>(const AudioRendererLimits&) const = default;
};

} // namespace pulp::playback
