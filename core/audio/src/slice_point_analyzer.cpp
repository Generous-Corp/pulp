#include <pulp/audio/slice_point_analyzer.hpp>

#include "audio_analysis_detail.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::audio {

namespace {

using detail::snap_to_zero_crossing;

int marker_source_priority(SliceMarkerSource source) noexcept {
    switch (source) {
        case SliceMarkerSource::LoopStart:
        case SliceMarkerSource::LoopEnd:
            return 5;
        case SliceMarkerSource::Manual:
        case SliceMarkerSource::Imported:
            return 4;
        case SliceMarkerSource::BeatGrid:
            return 3;
        case SliceMarkerSource::Silence:
            return 2;
        case SliceMarkerSource::Onset:
            return 1;
    }
    return 0;
}

bool better_marker(SliceMarker candidate, SliceMarker existing) noexcept {
    if (candidate.confidence != existing.confidence) {
        return candidate.confidence > existing.confidence;
    }
    return marker_source_priority(candidate.source) >
           marker_source_priority(existing.source);
}

void add_marker(std::vector<SliceMarker>& markers,
                SliceMarker marker,
                std::uint64_t min_spacing) {
    if (markers.empty()) {
        markers.push_back(marker);
        return;
    }

    auto& previous = markers.back();
    if (marker.frame < previous.frame + min_spacing) {
        if (previous.frame == 0) return;
        if (better_marker(marker, previous)) previous = marker;
        return;
    }

    markers.push_back(marker);
}

}  // namespace

SlicePointAnalysisResult SlicePointAnalyzer::analyze(
    BufferView<const float> source,
    std::span<const OnsetMarker> onsets,
    const SlicePointAnalysisConfig& config) const {
    SlicePointAnalysisResult result;
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source.num_channels() == 0 || source_frames == 0 ||
        !(config.source_sample_rate > 0.0) ||
        !std::isfinite(config.source_sample_rate)) {
        return result;
    }

    result.map.source_generation = config.source_generation;
    result.map.source_frames = source_frames;
    result.map.source_sample_rate = config.source_sample_rate;

    SliceMarker origin;
    origin.frame = 0;
    origin.confidence = 1.0;
    origin.source = SliceMarkerSource::Manual;
    result.map.markers.push_back(origin);

    std::vector<SliceMarker> candidates;
    candidates.reserve(onsets.size() + config.additional_markers.size());
    for (const auto& onset : onsets) {
        candidates.push_back({onset.frame, onset.confidence, SliceMarkerSource::Onset});
    }
    for (const auto& marker : config.additional_markers) {
        candidates.push_back(marker);
    }
    std::vector<SliceMarker> snapped_candidates;
    snapped_candidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.frame == 0 || candidate.frame >= source_frames) continue;
        auto frame = candidate.frame;
        if (config.snap_to_zero_crossing) {
            frame = snap_to_zero_crossing(source, frame, config.snap_radius_frames);
        }
        if (frame == 0 || frame >= source_frames) continue;
        snapped_candidates.push_back({frame, candidate.confidence, candidate.source});
    }
    std::sort(snapped_candidates.begin(), snapped_candidates.end(), [](const auto& a, const auto& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return marker_source_priority(a.source) > marker_source_priority(b.source);
    });

    for (const auto& candidate : snapped_candidates) {
        add_marker(result.map.markers, candidate, config.min_slice_frames);
    }

    if (result.map.markers.size() == 1) {
        SliceRegion region;
        region.start_frame = 0;
        region.end_frame = source_frames;
        region.marker_index = 0;
        result.map.regions.push_back(region);
        result.ok = validate_slice_map(result.map);
        return result;
    }

    for (std::size_t i = 0; i < result.map.markers.size(); ++i) {
        const auto start = result.map.markers[i].frame;
        const auto end =
            i + 1 < result.map.markers.size()
                ? result.map.markers[i + 1].frame
                : source_frames;
        if (end <= start) continue;

        SliceRegion region;
        region.start_frame = start;
        region.end_frame = end;
        region.marker_index = static_cast<std::uint32_t>(i);
        result.map.regions.push_back(region);
    }

    result.ok = validate_slice_map(result.map);
    return result;
}

}  // namespace pulp::audio
