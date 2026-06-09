#include <pulp/audio/loop_point_analyzer.hpp>

#include "audio_analysis_detail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::audio {

namespace {

using detail::average_abs_delta;
using detail::average_abs_sample;
using detail::aggregate_sample;
using detail::clamp_frame;
using detail::range_begin;
using detail::range_end;
using detail::rms_window;
using detail::slope_at;
using detail::window_correlation;
using detail::zero_crossing_cost;

std::uint64_t seam_frame(LoopRegion region) noexcept {
    return region.end_frame == 0 ? 0 : region.end_frame - 1;
}

std::uint64_t endpoint_compare_frame(LoopRegion region,
                                     std::uint64_t source_frames) noexcept {
    if (region.end_frame < source_frames) return region.end_frame;
    return seam_frame(region);
}

double wrap_slope(BufferView<const float> source,
                  const LoopRegion& region) noexcept {
    return aggregate_sample(source, region.start_frame) -
           aggregate_sample(source, seam_frame(region));
}

double transient_penalty(BufferView<const float> source,
                         std::uint64_t frame,
                         std::uint64_t radius) noexcept {
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source_frames < 2) return 0.0;
    const auto begin = range_begin(frame, radius);
    const auto end = range_end(frame, radius, source_frames);
    double peak = 0.0;
    for (std::uint64_t i = begin; i <= end && i + 1 < source_frames; ++i) {
        peak = std::max(peak, std::abs(slope_at(source, i)));
    }
    return peak;
}

double average_slope_delta(BufferView<const float> source,
                           std::uint64_t a,
                           std::uint64_t b) noexcept {
    const auto frames = static_cast<std::uint64_t>(source.num_samples());
    if (source.num_channels() == 0 || frames < 2) return 0.0;
    const auto index_a = clamp_frame(a, frames);
    const auto index_b = clamp_frame(b, frames);
    double sum = 0.0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        const auto* channel = source.channel_ptr(ch);
        const auto slope_a = index_a + 1 < frames
            ? static_cast<double>(channel[index_a + 1]) - static_cast<double>(channel[index_a])
            : static_cast<double>(channel[index_a]) - static_cast<double>(channel[index_a - 1]);
        const auto slope_b = index_b + 1 < frames
            ? static_cast<double>(channel[index_b + 1]) - static_cast<double>(channel[index_b])
            : static_cast<double>(channel[index_b]) - static_cast<double>(channel[index_b - 1]);
        sum += std::abs(slope_a - slope_b);
    }
    return sum / static_cast<double>(source.num_channels());
}

double slope_direction_mismatch(BufferView<const float> source,
                                std::uint64_t a,
                                std::uint64_t b) noexcept {
    const auto frames = static_cast<std::uint64_t>(source.num_samples());
    if (source.num_channels() == 0 || frames < 2) return 0.0;
    const auto index_a = clamp_frame(a, frames);
    const auto index_b = clamp_frame(b, frames);
    std::uint64_t comparable = 0;
    std::uint64_t mismatched = 0;
    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        const auto* channel = source.channel_ptr(ch);
        const auto slope_a = index_a + 1 < frames
            ? static_cast<double>(channel[index_a + 1]) - static_cast<double>(channel[index_a])
            : static_cast<double>(channel[index_a]) - static_cast<double>(channel[index_a - 1]);
        const auto slope_b = index_b + 1 < frames
            ? static_cast<double>(channel[index_b + 1]) - static_cast<double>(channel[index_b])
            : static_cast<double>(channel[index_b]) - static_cast<double>(channel[index_b - 1]);
        if (std::abs(slope_a) <= 1.0e-12 || std::abs(slope_b) <= 1.0e-12) {
            continue;
        }
        ++comparable;
        if (slope_a * slope_b < 0.0) ++mismatched;
    }
    return comparable == 0 ? 0.0
                           : static_cast<double>(mismatched) /
                                 static_cast<double>(comparable);
}

std::uint64_t frame_distance(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a - b : b - a;
}

bool snap_better(const LoopEndpointSnapResult& candidate,
                 const LoopEndpointSnapResult& best) noexcept {
    if (!best.valid) return true;
    if (candidate.cost != best.cost) return candidate.cost < best.cost;
    const auto candidate_distance =
        frame_distance(candidate.frame, candidate.hint_frame);
    const auto best_distance = frame_distance(best.frame, best.hint_frame);
    if (candidate_distance != best_distance) {
        return candidate_distance < best_distance;
    }
    return candidate.frame < best.frame;
}

std::uint64_t clamp_boundary_frame(std::uint64_t frame,
                                   std::uint64_t source_frames,
                                   LoopEndpointRole role) noexcept {
    if (source_frames == 0) return 0;
    if (role == LoopEndpointRole::ExclusiveEndFrame) {
        return std::min(frame, source_frames);
    }
    return clamp_frame(frame, source_frames);
}

std::uint64_t scored_endpoint_frame(std::uint64_t boundary_frame,
                                    std::uint64_t source_frames,
                                    LoopEndpointRole role) noexcept {
    if (source_frames == 0) return 0;
    if (role == LoopEndpointRole::ExclusiveEndFrame &&
        boundary_frame == source_frames) {
        return source_frames - 1;
    }
    return clamp_frame(boundary_frame, source_frames);
}

std::uint64_t boundary_range_end(std::uint64_t hint,
                                 std::uint64_t radius,
                                 std::uint64_t source_frames,
                                 LoopEndpointRole role) noexcept {
    if (source_frames == 0) return 0;
    const auto max_boundary = role == LoopEndpointRole::ExclusiveEndFrame
        ? source_frames
        : source_frames - 1;
    hint = std::min(hint, max_boundary);
    if (radius > max_boundary - hint) return max_boundary;
    return hint + radius;
}

std::uint64_t comparison_window_start(const LoopRegion& region,
                                      std::uint64_t source_frames,
                                      std::uint64_t window_frames) noexcept {
    (void) source_frames;
    if (region.end_frame >= window_frames) {
        return region.end_frame - window_frames;
    }
    return region.start_frame;
}

void retain_candidate(std::vector<LoopPointCandidate>& candidates,
                      LoopPointCandidate candidate,
                      std::size_t max_candidates) {
    if (!candidate.valid || max_candidates == 0) return;
    const auto by_score = [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score < b.score;
        if (a.region.start_frame != b.region.start_frame) {
            return a.region.start_frame < b.region.start_frame;
        }
        return a.region.end_frame < b.region.end_frame;
    };

    if (candidates.size() < max_candidates) {
        candidates.push_back(candidate);
        std::sort(candidates.begin(), candidates.end(), by_score);
        return;
    }

    if (by_score(candidate, candidates.back())) {
        candidates.back() = candidate;
        std::sort(candidates.begin(), candidates.end(), by_score);
    }
}

void add_unique_frame(std::vector<std::uint64_t>& frames,
                      std::uint64_t frame,
                      std::uint64_t source_frames) {
    if (source_frames == 0) return;
    frame = std::min(frame, source_frames);
    if (std::find(frames.begin(), frames.end(), frame) == frames.end()) {
        frames.push_back(frame);
    }
}

void add_unique_frame_bounded(std::vector<std::uint64_t>& frames,
                              std::uint64_t frame,
                              std::uint64_t source_frames,
                              std::size_t max_frames) {
    if (frames.size() >= max_frames) return;
    add_unique_frame(frames, frame, source_frames);
}

std::size_t axis_candidate_limit(std::uint64_t max_evaluations) noexcept {
    const auto root = std::ceil(std::sqrt(static_cast<long double>(max_evaluations)));
    if (root >= static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
        return std::numeric_limits<std::size_t>::max();
    }
    return std::max<std::size_t>(1, static_cast<std::size_t>(root));
}

void add_range_frames_bounded(std::vector<std::uint64_t>& frames,
                              std::uint64_t begin,
                              std::uint64_t end,
                              std::uint64_t source_frames,
                              std::size_t max_frames) {
    if (begin > end || frames.size() >= max_frames) return;

    const auto remaining = max_frames - frames.size();
    const auto span = end - begin + 1;
    if (span <= remaining) {
        for (std::uint64_t frame = begin; frame <= end; ++frame) {
            add_unique_frame_bounded(frames, frame, source_frames, max_frames);
        }
        return;
    }

    if (remaining == 1) {
        add_unique_frame_bounded(frames, end, source_frames, max_frames);
        return;
    }

    const auto last = span - 1;
    for (std::size_t i = 0; i < remaining; ++i) {
        const auto offset =
            (last * static_cast<std::uint64_t>(i)) /
            static_cast<std::uint64_t>(remaining - 1);
        add_unique_frame_bounded(frames, begin + offset, source_frames, max_frames);
    }
}

void add_anchor_marker(std::vector<std::uint64_t>& start_candidates,
                       std::vector<std::uint64_t>& end_candidates,
                       const SliceMarker& marker,
                       std::uint64_t source_frames,
                       std::size_t max_axis_candidates) {
    if (marker.frame >= source_frames) return;
    switch (marker.source) {
        case SliceMarkerSource::LoopStart:
            add_unique_frame_bounded(start_candidates,
                                     marker.frame,
                                     source_frames,
                                     max_axis_candidates);
            break;
        case SliceMarkerSource::LoopEnd:
            if (marker.frame > 0) {
                add_unique_frame_bounded(end_candidates,
                                         marker.frame,
                                         source_frames,
                                         max_axis_candidates);
            }
            break;
        case SliceMarkerSource::Manual:
        case SliceMarkerSource::Imported:
            add_unique_frame_bounded(start_candidates,
                                     marker.frame,
                                     source_frames,
                                     max_axis_candidates);
            if (marker.frame > 0) {
                add_unique_frame_bounded(end_candidates,
                                         marker.frame,
                                         source_frames,
                                         max_axis_candidates);
            }
            break;
        case SliceMarkerSource::Onset:
        case SliceMarkerSource::BeatGrid:
        case SliceMarkerSource::Silence:
            add_unique_frame_bounded(start_candidates,
                                     marker.frame,
                                     source_frames,
                                     max_axis_candidates);
            break;
    }
}

}  // namespace

LoopEndpointSnapResult LoopPointAnalyzer::snap_endpoint(
    BufferView<const float> source,
    std::uint64_t hint_frame,
    LoopSnapPolicy policy,
    std::uint64_t search_radius_frames,
    std::uint64_t reference_frame,
    LoopEndpointRole role) noexcept {
    LoopEndpointSnapResult best;
    best.hint_frame = hint_frame;
    best.policy = policy;
    best.role = role;

    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source.num_channels() == 0 || source_frames == 0) return best;

    const auto hint = clamp_boundary_frame(hint_frame, source_frames, role);
    const auto scored_hint = scored_endpoint_frame(hint, source_frames, role);
    best.hint_frame = hint;
    if (policy == LoopSnapPolicy::None) {
        best.valid = true;
        best.frame = hint;
        best.scored_frame = scored_hint;
        best.cost = 0.0;
        return best;
    }

    const bool has_reference = reference_frame < source_frames;
    const auto reference = has_reference ? clamp_frame(reference_frame, source_frames) : hint;
    const auto begin = range_begin(hint, search_radius_frames);
    const auto end = boundary_range_end(hint, search_radius_frames, source_frames, role);

    for (std::uint64_t boundary_frame = begin; boundary_frame <= end; ++boundary_frame) {
        const auto frame = scored_endpoint_frame(boundary_frame, source_frames, role);
        LoopEndpointSnapResult candidate;
        candidate.valid = true;
        candidate.hint_frame = hint;
        candidate.frame = boundary_frame;
        candidate.scored_frame = frame;
        candidate.policy = policy;
        candidate.role = role;
        candidate.zero_crossing_cost = zero_crossing_cost(source, frame);

        candidate.value_delta = has_reference
            ? average_abs_delta(source, frame, reference)
            : average_abs_sample(source, frame);
        candidate.slope_delta = has_reference
            ? average_slope_delta(source, frame, reference)
            : std::abs(slope_at(source, frame));

        switch (policy) {
            case LoopSnapPolicy::NearestZeroCrossing:
                candidate.cost = candidate.zero_crossing_cost;
                break;
            case LoopSnapPolicy::SlopeMatchedZeroCrossing:
                candidate.cost = candidate.zero_crossing_cost;
                if (has_reference) {
                    candidate.cost += slope_direction_mismatch(source, frame, reference);
                }
                if (has_reference) candidate.cost += 0.1 * candidate.slope_delta;
                break;
            case LoopSnapPolicy::ValueDirection:
                candidate.cost = has_reference
                    ? candidate.value_delta + candidate.slope_delta
                    : frame_distance(frame, hint);
                break;
            case LoopSnapPolicy::None:
                candidate.cost = 0.0;
                break;
        }

        if (std::isfinite(candidate.cost) && snap_better(candidate, best)) {
            best = candidate;
        }
    }

    return best;
}

LoopPointCandidate LoopPointAnalyzer::score_candidate(
    BufferView<const float> source,
    const LoopRegion& region,
    std::uint64_t window_frames) noexcept {
    LoopPointCandidate candidate;
    candidate.region = region;

    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (!validate_loop_region(region, source_frames).ok ||
        source.num_channels() == 0) {
        return candidate;
    }

    const auto loop_length = region.end_frame - region.start_frame;
    window_frames = std::max<std::uint64_t>(1, window_frames);
    window_frames = std::min(window_frames, loop_length);
    const auto seam = seam_frame(region);
    const auto endpoint_compare = endpoint_compare_frame(region, source_frames);
    const auto compare_start =
        comparison_window_start(region, source_frames, window_frames);

    candidate.endpoint_delta =
        average_abs_delta(source, region.start_frame, endpoint_compare);
    candidate.slope_delta =
        std::abs(slope_at(source, region.start_frame) - wrap_slope(source, region));
    candidate.rms_delta =
        std::abs(rms_window(source, region.start_frame, window_frames) -
                 rms_window(source, compare_start, window_frames));
    candidate.correlation =
        window_correlation(source, region.start_frame, compare_start, window_frames);
    candidate.transient_penalty =
        0.5 * (transient_penalty(source, region.start_frame, 4) +
               transient_penalty(source, seam, 4));
    candidate.zero_crossing_cost =
        zero_crossing_cost(source, region.start_frame) +
        zero_crossing_cost(source, endpoint_compare);

    const auto correlation_penalty = 1.0 - std::max(0.0, candidate.correlation);
    double snap_penalty = 0.0;
    if (region.snap_policy == LoopSnapPolicy::NearestZeroCrossing ||
        region.snap_policy == LoopSnapPolicy::SlopeMatchedZeroCrossing) {
        snap_penalty += candidate.zero_crossing_cost;
    }
    if (region.snap_policy == LoopSnapPolicy::SlopeMatchedZeroCrossing &&
        slope_at(source, region.start_frame) * wrap_slope(source, region) < 0.0) {
        snap_penalty += 1.0;
    }

    candidate.score = 4.0 * candidate.endpoint_delta +
                      2.0 * candidate.slope_delta +
                      candidate.rms_delta +
                      2.0 * correlation_penalty +
                      0.25 * candidate.transient_penalty +
                      snap_penalty;
    candidate.valid = std::isfinite(candidate.score);
    return candidate;
}

LoopPointAnalysisResult LoopPointAnalyzer::analyze(
    BufferView<const float> source,
    const LoopPointAnalysisConfig& config) const {
    LoopPointAnalysisResult result;
    const auto source_frames = static_cast<std::uint64_t>(source.num_samples());
    if (source.num_channels() == 0 || source_frames == 0 ||
        !(config.source_sample_rate > 0.0) ||
        !std::isfinite(config.source_sample_rate) ||
        config.max_candidates == 0 || config.max_evaluations == 0) {
        return result;
    }

    const auto start_hint = clamp_frame(config.start_hint_frame, source_frames);
    const auto default_end =
        config.end_hint_frame == 0 ? source_frames : config.end_hint_frame;
    const auto end_hint = std::min(default_end, source_frames);
    const auto start_begin_frame = range_begin(start_hint, config.search_radius_frames);
    const auto start_end_frame = range_end(start_hint, config.search_radius_frames, source_frames);
    const auto end_begin_frame = std::max<std::uint64_t>(
        1, range_begin(end_hint, config.search_radius_frames));
    const auto end_end_frame = std::min<std::uint64_t>(
        source_frames, end_hint + std::min(config.search_radius_frames, source_frames));

    const auto max_axis_candidates = axis_candidate_limit(config.max_evaluations);
    std::vector<std::uint64_t> start_candidates;
    std::vector<std::uint64_t> end_candidates;
    const auto reserve_hint = std::min<std::size_t>(max_axis_candidates, 4096);
    start_candidates.reserve(reserve_hint);
    end_candidates.reserve(reserve_hint);

    add_unique_frame_bounded(start_candidates, start_hint, source_frames, max_axis_candidates);
    add_unique_frame_bounded(end_candidates, end_hint, source_frames, max_axis_candidates);
    const auto start_reference =
        scored_endpoint_frame(end_hint,
                              source_frames,
                              LoopEndpointRole::ExclusiveEndFrame);
    const auto snapped_start = snap_endpoint(source,
                                             start_hint,
                                             config.snap_policy,
                                             config.search_radius_frames,
                                             start_reference);
    if (snapped_start.valid) {
        add_unique_frame_bounded(start_candidates,
                                 snapped_start.frame,
                                 source_frames,
                                 max_axis_candidates);
    }
    const auto snapped_end = snap_endpoint(source,
                                           end_hint,
                                           config.snap_policy,
                                           config.search_radius_frames,
                                           snapped_start.valid ? snapped_start.scored_frame : start_hint,
                                           LoopEndpointRole::ExclusiveEndFrame);
    if (snapped_end.valid) {
        add_unique_frame_bounded(end_candidates,
                                 snapped_end.frame,
                                 source_frames,
                                 max_axis_candidates);
    }
    for (const auto& marker : config.anchor_markers) {
        add_anchor_marker(start_candidates,
                          end_candidates,
                          marker,
                          source_frames,
                          max_axis_candidates);
    }
    add_range_frames_bounded(start_candidates,
                             start_begin_frame,
                             start_end_frame,
                             source_frames,
                             max_axis_candidates);
    add_range_frames_bounded(end_candidates,
                             end_begin_frame,
                             end_end_frame,
                             source_frames,
                             max_axis_candidates);
    std::sort(start_candidates.begin(), start_candidates.end());
    std::sort(end_candidates.begin(), end_candidates.end());

    result.candidates.reserve(config.max_candidates);
    std::uint64_t considered = 0;
    bool stop = false;
    for (const auto start : start_candidates) {
        for (const auto end : end_candidates) {
            if (considered >= config.max_evaluations) {
                result.evaluation_limit_reached = true;
                stop = true;
                break;
            }
            ++considered;

            LoopRegion region;
            region.start_frame = start;
            region.end_frame = end;
            region.crossfade_frames = config.crossfade_frames;
            region.source_sample_rate = config.source_sample_rate;
            region.snap_policy = config.snap_policy;

            if (!validate_loop_region(region, source_frames, config.min_loop_frames).ok) {
                continue;
            }

            auto candidate = score_candidate(source, region, config.window_frames);
            if (!candidate.valid) continue;
            ++result.evaluated_candidates;
            retain_candidate(result.candidates, candidate, config.max_candidates);
        }
        if (stop) break;
    }

    result.ok = !result.candidates.empty();
    return result;
}

}  // namespace pulp::audio
