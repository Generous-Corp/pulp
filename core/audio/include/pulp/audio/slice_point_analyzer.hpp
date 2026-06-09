#pragma once

#include <cstdint>
#include <span>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

struct SlicePointAnalysisConfig {
    std::uint64_t source_generation = 0;
    double source_sample_rate = 0.0;
    std::uint64_t min_slice_frames = 256;
    std::uint64_t snap_radius_frames = 64;
    bool snap_to_zero_crossing = true;
    std::span<const SliceMarker> additional_markers{};
};

struct SlicePointAnalysisResult {
    bool ok = false;
    SliceMap map;
};

class SlicePointAnalyzer {
public:
    // Off-real-time/background analysis. Merges built-in/package onsets with
    // optional beat-grid, silence, manual, imported, and loop-boundary markers.
    SlicePointAnalysisResult analyze(BufferView<const float> source,
                                     std::span<const OnsetMarker> onsets,
                                     const SlicePointAnalysisConfig& config) const;
};

}  // namespace pulp::audio
