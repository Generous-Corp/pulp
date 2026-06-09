#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <pulp/audio/analyzer_provider.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

struct SliceMarkerClassification {
    std::uint32_t marker_index = 0;
    TransientClass transient_class = TransientClass::Unknown;
    double confidence = 0.0;
    AnalyzerProvenance provenance;
};

struct SlicePointAnalysisConfig {
    std::uint64_t source_generation = 0;
    double source_sample_rate = 0.0;
    std::uint64_t min_slice_frames = 256;
    std::uint64_t snap_radius_frames = 64;
    bool snap_to_zero_crossing = true;
    std::span<const SliceMarker> additional_markers{};
    // Sidecar provenance over the `onsets` span. marker_index refers to the
    // source onset index and is remapped to the final debounced slice marker.
    std::span<const AnalyzerMarkerProvenance> onset_provenance{};
    // Optional control-thread classifier output associated with final markers.
    std::span<const TransientClassification> transient_classifications{};
    std::uint64_t transient_match_radius_frames = 256;
};

struct SlicePointAnalysisResult {
    bool ok = false;
    SliceMap map;
    std::vector<AnalyzerMarkerProvenance> marker_provenance;
    std::vector<SliceMarkerClassification> marker_classifications;
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
