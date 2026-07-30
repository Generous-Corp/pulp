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
    std::uint32_t max_sample_rate_converters = 64u;
    // Conservative retained-allocation charge, including container capacity,
    // shared ownership, and per-allocation bookkeeping.
    std::uint64_t max_sample_rate_converter_bytes = 256u * 1024u * 1024u;
    std::uint64_t max_offline_stretch_input_frames = 100'000'000u;
    std::uint64_t max_offline_stretch_output_frames = 100'000'000u;
    // Offline Stretch uses scalar double planar input/output scratch to keep
    // separate compiles bit-identical before bounded float artifact sealing.
    std::uint64_t max_offline_stretch_input_bytes = 1024u * 1024u * 1024u;
    std::uint64_t max_offline_stretch_scratch_allocation_bytes = 256u * 1024u * 1024u;
    std::uint64_t max_offline_stretch_output_bytes = 1024u * 1024u * 1024u;
    std::uint64_t max_offline_stretch_artifact_bytes = 512u * 1024u * 1024u;
    std::uint64_t max_offline_stretch_cache_bytes = 2u << 30u;
    std::uint32_t max_offline_stretch_artifacts = 256u;
    std::uint32_t offline_stretch_max_block_frames = 256u;
    std::uint32_t offline_stretch_algorithm_version = 1u;
    float offline_stretch_max_time_ratio = 16.0f;
    constexpr auto operator<=>(const AudioRendererLimits&) const = default;
};

} // namespace pulp::playback
