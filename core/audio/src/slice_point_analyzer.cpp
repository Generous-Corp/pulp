#include <pulp/audio/slice_point_analyzer.hpp>

#include "audio_analysis_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace pulp::audio {

namespace {

using detail::snap_to_zero_crossing;

struct CandidateSliceMarker {
    SliceMarker marker;
    std::uint32_t onset_index = 0;
    bool has_onset_index = false;
};

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

bool valid_marker_confidence(double confidence) noexcept {
    return std::isfinite(confidence) && confidence >= 0.0;
}

bool sign_transition(BufferView<const float> source,
                     std::uint64_t frame) noexcept {
    if (frame == 0 || frame >= source.num_samples()) return false;
    const auto previous = detail::aggregate_sample(source, frame - 1);
    const auto current = detail::aggregate_sample(source, frame);
    return (previous <= 0.0) != (current <= 0.0);
}

std::uint64_t snap_to_sign_transition(BufferView<const float> source,
                                      std::uint64_t frame,
                                      std::uint64_t radius) noexcept {
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source_frames < 2) return frame;
    frame = std::min(frame, source_frames - 1);
    if (sign_transition(source, frame)) return frame;
    for (std::uint64_t distance = 1; distance <= radius; ++distance) {
        if (frame > distance && sign_transition(source, frame - distance)) {
            return frame - distance;
        }
        if (frame + distance < source_frames &&
            sign_transition(source, frame + distance)) {
            return frame + distance;
        }
    }
    return frame;
}

bool better_marker(SliceMarker candidate, SliceMarker existing) noexcept {
    if (candidate.confidence != existing.confidence) {
        return candidate.confidence > existing.confidence;
    }
    return marker_source_priority(candidate.source) >
           marker_source_priority(existing.source);
}

std::uint64_t frame_distance(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a - b : b - a;
}

void add_candidate(std::vector<CandidateSliceMarker>& markers,
                   CandidateSliceMarker candidate,
                   std::uint64_t min_spacing) {
    if (markers.empty()) {
        markers.push_back(candidate);
        return;
    }

    auto& previous = markers.back();
    if (candidate.marker.frame - previous.marker.frame < min_spacing) {
        if (previous.marker.frame == 0) return;
        if (better_marker(candidate.marker, previous.marker)) previous = candidate;
        return;
    }

    markers.push_back(candidate);
}

const AnalyzerMarkerProvenance* provenance_for_onset(
    std::span<const AnalyzerMarkerProvenance> provenance,
    std::uint32_t onset_index) noexcept {
    const auto it = std::lower_bound(
        provenance.begin(), provenance.end(), onset_index, [](const auto& item, auto index) {
            return item.marker_index < index;
        });
    if (it != provenance.end() && it->marker_index == onset_index) return &*it;
    return nullptr;
}

void append_marker_provenance(SlicePointAnalysisResult& result,
                              std::span<const CandidateSliceMarker> selected,
                              std::span<const AnalyzerMarkerProvenance> onset_provenance) {
    result.marker_provenance.reserve(onset_provenance.size());
    for (std::size_t marker_index = 0;
         marker_index < selected.size() &&
         marker_index <= std::numeric_limits<std::uint32_t>::max();
         ++marker_index) {
        const auto& candidate = selected[marker_index];
        if (!candidate.has_onset_index) continue;
        const auto* provenance = provenance_for_onset(onset_provenance, candidate.onset_index);
        if (!provenance) continue;

        AnalyzerMarkerProvenance remapped = *provenance;
        remapped.marker_index = static_cast<std::uint32_t>(marker_index);
        result.marker_provenance.push_back(std::move(remapped));
    }
}

bool nearest_marker_index_for_classification(
    const SlicePointAnalysisResult& result,
    const TransientClassification& classification,
    std::uint64_t match_radius_frames,
    std::size_t& marker_index_out) noexcept {
    std::size_t best_index = 0;
    auto best_distance = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t marker_index = 0; marker_index < result.map.markers.size();
         ++marker_index) {
        const auto distance =
            frame_distance(classification.frame, result.map.markers[marker_index].frame);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = marker_index;
        }
    }
    if (best_distance > match_radius_frames ||
        best_index > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    marker_index_out = best_index;
    return true;
}

bool mapped_marker_index_for_classification(
    const TransientClassification& classification,
    std::span<const std::uint32_t> candidate_marker_indices,
    std::size_t marker_count,
    std::size_t& marker_index_out) noexcept {
    if (!classification.has_candidate_index || candidate_marker_indices.empty()) return false;
    if (classification.candidate_index >= candidate_marker_indices.size()) return false;

    const auto marker_index = candidate_marker_indices[classification.candidate_index];
    if (marker_index == kUnmappedTransientCandidateMarkerIndex) return false;
    if (marker_index >= marker_count) return false;

    marker_index_out = static_cast<std::size_t>(marker_index);
    return true;
}

bool classification_has_explicit_candidate_mapping(
    const TransientClassification& classification,
    std::span<const std::uint32_t> candidate_marker_indices) noexcept {
    return classification.has_candidate_index && !candidate_marker_indices.empty();
}

void append_marker_classifications(
    SlicePointAnalysisResult& result,
    std::span<const TransientClassification> classifications,
    std::span<const std::uint32_t> candidate_marker_indices,
    std::uint64_t match_radius_frames) {
    if (classifications.empty() || result.map.markers.empty()) return;

    std::vector<SliceMarkerClassification> best(result.map.markers.size());
    std::vector<bool> have(result.map.markers.size(), false);

    for (const auto& classification : classifications) {
        if (classification.frame >= result.map.source_frames ||
            !std::isfinite(classification.confidence) || classification.confidence < 0.0 ||
            classification.provenance.provider_id.empty()) {
            continue;
        }

        std::size_t marker_index = 0;
        const auto has_mapping = classification_has_explicit_candidate_mapping(
            classification,
            candidate_marker_indices);
        const auto resolved_by_mapping = mapped_marker_index_for_classification(
            classification,
            candidate_marker_indices,
            result.map.markers.size(),
            marker_index);
        if (has_mapping && !resolved_by_mapping) continue;
        if (!resolved_by_mapping &&
            !nearest_marker_index_for_classification(result,
                                                     classification,
                                                     match_radius_frames,
                                                     marker_index)) {
            continue;
        }

        auto candidate = SliceMarkerClassification{};
        candidate.marker_index = static_cast<std::uint32_t>(marker_index);
        candidate.transient_class = classification.transient_class;
        candidate.confidence = classification.confidence;
        candidate.provenance = classification.provenance;

        if (!have[marker_index] || candidate.confidence > best[marker_index].confidence) {
            best[marker_index] = std::move(candidate);
            have[marker_index] = true;
        }
    }

    for (std::size_t marker_index = 0; marker_index < best.size(); ++marker_index) {
        if (have[marker_index]) result.marker_classifications.push_back(std::move(best[marker_index]));
    }
}

}  // namespace

SlicePointAnalysisResult SlicePointAnalyzer::analyze(
    BufferView<const float> source,
    std::span<const OnsetMarker> onsets,
    const SlicePointAnalysisConfig& config) const {
    return analyze(source, onsets, config, {});
}

SlicePointAnalysisResult SlicePointAnalyzer::analyze(
    BufferView<const float> source,
    std::span<const OnsetMarker> onsets,
    const SlicePointAnalysisConfig& config,
    const SliceSelectionOptions& options) const {
    SlicePointAnalysisResult result;
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source.num_channels() == 0 || source_frames == 0 ||
        !(config.source_sample_rate > 0.0) ||
        !std::isfinite(config.source_sample_rate) ||
        !validate_analyzer_marker_provenance(onsets.size(), config.onset_provenance)) {
        return result;
    }

    result.map.source_generation = config.source_generation;
    result.map.source_frames = source_frames;
    result.map.source_sample_rate = config.source_sample_rate;

    CandidateSliceMarker origin;
    origin.marker.frame = 0;
    origin.marker.confidence = 1.0;
    origin.marker.source = SliceMarkerSource::Manual;

    std::vector<CandidateSliceMarker> selected;
    selected.push_back(origin);

    std::vector<CandidateSliceMarker> candidates;
    candidates.reserve(onsets.size() + config.additional_markers.size());
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        if (i > std::numeric_limits<std::uint32_t>::max()) break;
        if (!valid_marker_confidence(onsets[i].confidence)) continue;
        CandidateSliceMarker candidate;
        candidate.marker = {onsets[i].frame, onsets[i].confidence, SliceMarkerSource::Onset};
        candidate.onset_index = static_cast<std::uint32_t>(i);
        candidate.has_onset_index = true;
        candidates.push_back(candidate);
    }
    for (const auto& marker : config.additional_markers) {
        if (!valid_marker_confidence(marker.confidence)) continue;
        CandidateSliceMarker candidate;
        candidate.marker = marker;
        candidates.push_back(candidate);
    }
    std::vector<CandidateSliceMarker> snapped_candidates;
    snapped_candidates.reserve(candidates.size());
    for (auto candidate : candidates) {
        if (candidate.marker.frame == 0 || candidate.marker.frame >= source_frames) continue;
        auto frame = candidate.marker.frame;
        if (config.snap_to_zero_crossing) {
            frame = options.snap_policy == SliceSnapPolicy::SignTransition
                        ? snap_to_sign_transition(source,
                                                  frame,
                                                  config.snap_radius_frames)
                        : snap_to_zero_crossing(source,
                                                frame,
                                                config.snap_radius_frames);
        }
        if (frame == 0 || frame >= source_frames) continue;
        candidate.marker.frame = frame;
        snapped_candidates.push_back(candidate);
    }
    const auto timeline_order = [](const auto& a, const auto& b) {
        if (a.marker.frame != b.marker.frame) return a.marker.frame < b.marker.frame;
        if (a.marker.confidence != b.marker.confidence) {
            return a.marker.confidence > b.marker.confidence;
        }
        return marker_source_priority(a.marker.source) >
               marker_source_priority(b.marker.source);
    };
    const auto maximum_cuts = options.max_regions == 0
                                  ? std::numeric_limits<std::size_t>::max()
                                  : static_cast<std::size_t>(options.max_regions - 1);

    if (options.candidate_selection == SliceCandidateSelection::StrongestConfidence) {
        std::stable_sort(snapped_candidates.begin(),
                         snapped_candidates.end(),
                         [](const auto& a, const auto& b) {
                             if (a.marker.confidence != b.marker.confidence) {
                                 return a.marker.confidence > b.marker.confidence;
                             }
                             const auto a_priority = marker_source_priority(a.marker.source);
                             const auto b_priority = marker_source_priority(b.marker.source);
                             if (a_priority != b_priority) return a_priority > b_priority;
                             return a.marker.frame < b.marker.frame;
                         });
        for (const auto& candidate : snapped_candidates) {
            if (selected.size() - 1 >= maximum_cuts) break;
            if (candidate.marker.frame < config.min_slice_frames ||
                source_frames - candidate.marker.frame < config.min_slice_frames) {
                continue;
            }
            const auto spaced = std::all_of(
                selected.begin(), selected.end(), [&](const auto& existing) {
                    return frame_distance(candidate.marker.frame,
                                          existing.marker.frame) >=
                           config.min_slice_frames;
                });
            if (spaced) selected.push_back(candidate);
        }
        std::sort(selected.begin(), selected.end(), timeline_order);
    } else {
        std::sort(snapped_candidates.begin(), snapped_candidates.end(), timeline_order);
        for (const auto& candidate : snapped_candidates) {
            if (selected.size() - 1 >= maximum_cuts) break;
            add_candidate(selected, candidate, config.min_slice_frames);
        }
    }

    result.map.markers.reserve(selected.size());
    for (const auto& candidate : selected) {
        result.map.markers.push_back(candidate.marker);
    }
    append_marker_provenance(result, selected, config.onset_provenance);

    if (result.map.markers.size() == 1) {
        SliceRegion region;
        region.start_frame = 0;
        region.end_frame = source_frames;
        region.marker_index = 0;
        result.map.regions.push_back(region);
        append_marker_classifications(result,
                                      config.transient_classifications,
                                      config.transient_candidate_marker_indices,
                                      config.transient_match_radius_frames);
        result.ok = validate_slice_map(result.map) &&
                    validate_analyzer_marker_provenance(result.map.markers.size(),
                                                        result.marker_provenance);
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

    append_marker_classifications(result,
                                  config.transient_classifications,
                                  config.transient_candidate_marker_indices,
                                  config.transient_match_radius_frames);
    result.ok = validate_slice_map(result.map) &&
                validate_analyzer_marker_provenance(result.map.markers.size(),
                                                    result.marker_provenance);
    return result;
}

}  // namespace pulp::audio
