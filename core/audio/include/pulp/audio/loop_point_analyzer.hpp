#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

struct LoopPointAnalysisConfig {
    std::uint64_t start_hint_frame = 0;
    std::uint64_t end_hint_frame = 0;
    std::uint64_t search_radius_frames = 256;
    std::uint64_t window_frames = 128;
    std::uint64_t min_loop_frames = 16;
    std::uint64_t crossfade_frames = 0;
    double source_sample_rate = 0.0;
    std::size_t max_candidates = 16;
    // Upper bound on start/end candidate pairs considered. This keeps
    // off-RT analysis predictable for long files or dense imported markers.
    std::uint64_t max_evaluations = 65536;
    LoopSnapPolicy snap_policy = LoopSnapPolicy::ValueDirection;
    std::span<const SliceMarker> anchor_markers{};
};

struct LoopPointCandidate {
    bool valid = false;
    LoopRegion region;
    double score = std::numeric_limits<double>::infinity();
    double endpoint_delta = 0.0;
    double slope_delta = 0.0;
    double rms_delta = 0.0;
    double correlation = 0.0;
    double transient_penalty = 0.0;
    double zero_crossing_cost = 0.0;
};

enum class LoopEndpointRole : std::uint8_t {
    StartFrame,
    ExclusiveEndFrame,
};

struct LoopEndpointSnapResult {
    bool valid = false;
    std::uint64_t hint_frame = 0;
    // Boundary frame in the caller's coordinate system. For ExclusiveEndFrame
    // this may equal source.num_samples().
    std::uint64_t frame = 0;
    // Actual sample frame used for value/slope/zero-crossing scoring.
    std::uint64_t scored_frame = 0;
    LoopSnapPolicy policy = LoopSnapPolicy::None;
    LoopEndpointRole role = LoopEndpointRole::StartFrame;
    double cost = std::numeric_limits<double>::infinity();
    double value_delta = 0.0;
    double slope_delta = 0.0;
    double zero_crossing_cost = 0.0;
};

struct LoopPointAnalysisResult {
    bool ok = false;
    bool evaluation_limit_reached = false;
    std::uint64_t evaluated_candidates = 0;
    std::vector<LoopPointCandidate> candidates;
};

class LoopPointAnalyzer {
public:
    // Off-real-time/background analysis. May allocate and may examine long
    // windows; never call from Processor::process().
    LoopPointAnalysisResult analyze(BufferView<const float> source,
                                    const LoopPointAnalysisConfig& config) const;

    static LoopPointCandidate score_candidate(BufferView<const float> source,
                                              const LoopRegion& region,
                                              std::uint64_t window_frames) noexcept;

    // Off-real-time helper for editor/importer workflows that need to move a
    // hinted boundary to a click-reducing frame without running a full loop
    // candidate search. `reference_frame` is a scored sample frame, not an
    // exclusive-end boundary; pass `scored_frame` from another snap result when
    // matching against an exclusive end.
    static LoopEndpointSnapResult snap_endpoint(
        BufferView<const float> source,
        std::uint64_t hint_frame,
        LoopSnapPolicy policy,
        std::uint64_t search_radius_frames,
        std::uint64_t reference_frame = std::numeric_limits<std::uint64_t>::max(),
        LoopEndpointRole role = LoopEndpointRole::StartFrame) noexcept;
};

}  // namespace pulp::audio
