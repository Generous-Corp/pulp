#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <pulp/audio/analyzer_provider.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

inline constexpr std::uint32_t kUnmappedTransientCandidateMarkerIndex =
    std::numeric_limits<std::uint32_t>::max();

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
    // Optional authoritative map from TransientClassification::candidate_index
    // to final SliceMap marker index for the same analysis generation. Use
    // kUnmappedTransientCandidateMarkerIndex for candidate frames that do not
    // have a surviving marker. When empty, classifications fall back to
    // nearest-frame matching. When present, classifications with explicit but
    // unresolvable candidate identity are dropped instead of falling back.
    std::span<const std::uint32_t> transient_candidate_marker_indices{};
    std::uint64_t transient_match_radius_frames = 256;
};

struct SlicePointAnalysisResult {
    bool ok = false;
    SliceMap map;
    std::vector<AnalyzerMarkerProvenance> marker_provenance;
    std::vector<SliceMarkerClassification> marker_classifications;
};

enum class SliceCandidateSelection : std::uint8_t {
    TimelineOrder,
    StrongestConfidence,
};

enum class SliceSnapPolicy : std::uint8_t {
    NearZero,
    SignTransition,
};

struct SliceSelectionOptions {
    // Includes the origin region. Zero leaves the region count unlimited.
    std::uint32_t max_regions = 0;
    SliceCandidateSelection candidate_selection =
        SliceCandidateSelection::TimelineOrder;
    SliceSnapPolicy snap_policy = SliceSnapPolicy::NearZero;
};

class SlicePointAnalyzer {
public:
    // Off-real-time/background analysis. Merges built-in/package onsets with
    // optional beat-grid, silence, manual, imported, and loop-boundary markers.
    SlicePointAnalysisResult analyze(BufferView<const float> source,
                                     std::span<const OnsetMarker> onsets,
                                     const SlicePointAnalysisConfig& config) const;
    SlicePointAnalysisResult analyze(BufferView<const float> source,
                                     std::span<const OnsetMarker> onsets,
                                     const SlicePointAnalysisConfig& config,
                                     const SliceSelectionOptions& options) const;
};

}  // namespace pulp::audio
