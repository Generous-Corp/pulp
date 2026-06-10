#pragma once

#include <cstdint>
#include <vector>

namespace pulp::audio {

enum class SliceMarkerSource : std::uint8_t {
    Onset,
    BeatGrid,
    Silence,
    Manual,
    Imported,
    LoopStart,
    LoopEnd,
};

struct SliceMarker {
    std::uint64_t frame = 0;
    double confidence = 0.0;
    SliceMarkerSource source = SliceMarkerSource::Manual;
};

struct SliceRegion {
    std::uint64_t start_frame = 0;
    std::uint64_t end_frame = 0;  // exclusive
    std::uint32_t marker_index = 0;
};

struct SliceMap {
    std::uint64_t source_generation = 0;
    std::uint64_t source_frames = 0;
    double source_sample_rate = 0.0;
    std::vector<SliceMarker> markers;
    std::vector<SliceRegion> regions;
};

bool validate_slice_map(const SliceMap& map) noexcept;

}  // namespace pulp::audio
