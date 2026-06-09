#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_point_analyzer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/slice_map.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::LoopEndpointRole;
using pulp::audio::LoopPointAnalysisConfig;
using pulp::audio::LoopPointAnalyzer;
using pulp::audio::LoopRegion;
using pulp::audio::LoopSnapPolicy;
using pulp::audio::LoopValidationStatus;
using pulp::audio::SliceMarker;
using pulp::audio::SliceMarkerSource;
using pulp::audio::validate_loop_region;

namespace {

constexpr double kPi = 3.14159265358979323846;

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

void fill_periodic_sine(Buffer<float>& buffer, std::uint64_t period_frames) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        const auto gain = ch == 0 ? 1.0 : 0.5;
        for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
            const auto phase =
                2.0 * kPi * static_cast<double>(i % period_frames) /
                static_cast<double>(period_frames);
            buffer.channel(ch)[i] = static_cast<float>(gain * std::sin(phase));
        }
    }
}

}  // namespace

TEST_CASE("LoopTypes validates ranges, duration, crossfade, and sample rate",
          "[audio][loop][analysis]") {
    LoopRegion region;
    region.start_frame = 10;
    region.end_frame = 110;
    region.crossfade_frames = 16;
    region.source_sample_rate = 48000.0;

    REQUIRE(validate_loop_region(region, 128, 16).ok);

    region.crossfade_frames = 60;
    auto validation = validate_loop_region(region, 128, 16);
    REQUIRE_FALSE(validation.ok);
    REQUIRE(validation.status == LoopValidationStatus::CrossfadeTooLong);

    region.crossfade_frames = 0;
    region.end_frame = 12;
    validation = validate_loop_region(region, 128, 16);
    REQUIRE_FALSE(validation.ok);
    REQUIRE(validation.status == LoopValidationStatus::TooShort);

    region.end_frame = 110;
    region.source_sample_rate = 0.0;
    validation = validate_loop_region(region, 128, 16);
    REQUIRE_FALSE(validation.ok);
    REQUIRE(validation.status == LoopValidationStatus::InvalidSampleRate);
}

TEST_CASE("LoopPointAnalyzer ranks a seamless periodic loop above a discontinuity",
          "[audio][loop][analysis]") {
    Buffer<float> source(2, 192);
    fill_periodic_sine(source, 64);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    LoopPointAnalysisConfig config;
    config.start_hint_frame = 0;
    config.end_hint_frame = 64;
    config.search_radius_frames = 2;
    config.window_frames = 32;
    config.min_loop_frames = 32;
    config.source_sample_rate = 48000.0;
    config.max_candidates = 8;

    LoopPointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, config);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.candidates.empty());
    REQUIRE(result.evaluated_candidates > 0);

    const auto& best = result.candidates.front();
    REQUIRE(best.valid);
    REQUIRE(best.region.end_frame - best.region.start_frame == 64);

    LoopRegion discontinuous = best.region;
    discontinuous.start_frame = 0;
    discontinuous.end_frame = 80;
    const auto bad =
        LoopPointAnalyzer::score_candidate(view, discontinuous, config.window_frames);
    REQUIRE(bad.valid);
    REQUIRE(best.score < bad.score);
}

TEST_CASE("LoopPointAnalyzer scores the exclusive loop-end seam",
          "[audio][loop][analysis]") {
    Buffer<float> source(1, 128);
    std::fill(source.channel(0).begin(), source.channel(0).end(), 0.0f);
    source.channel(0)[63] = 1.0f;
    source.channel(0)[64] = 0.0f;  // Continuation after the loop, not played.
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    LoopRegion false_continuation_match;
    false_continuation_match.start_frame = 0;
    false_continuation_match.end_frame = 64;
    false_continuation_match.source_sample_rate = 48000.0;

    LoopRegion seamless;
    seamless.start_frame = 0;
    seamless.end_frame = 32;
    seamless.source_sample_rate = 48000.0;

    const auto false_score =
        LoopPointAnalyzer::score_candidate(view, false_continuation_match, 16);
    const auto seamless_score =
        LoopPointAnalyzer::score_candidate(view, seamless, 16);

    REQUIRE(false_score.valid);
    REQUIRE(seamless_score.valid);
    REQUIRE(false_score.slope_delta > 0.9);
    REQUIRE(seamless_score.score < false_score.score);
}

TEST_CASE("LoopPointAnalyzer caps broad candidate searches",
          "[audio][loop][analysis]") {
    Buffer<float> source(1, 256);
    fill_periodic_sine(source, 64);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    LoopPointAnalysisConfig config;
    config.start_hint_frame = 0;
    config.end_hint_frame = 64;
    config.search_radius_frames = 64;
    config.window_frames = 8;
    config.min_loop_frames = 1;
    config.source_sample_rate = 48000.0;
    config.max_candidates = 4;
    config.max_evaluations = 3;

    LoopPointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, config);
    REQUIRE(result.evaluation_limit_reached);
    REQUIRE(result.evaluated_candidates <= config.max_evaluations);
}

TEST_CASE("LoopPointAnalyzer can prefer slope-matched zero crossings",
          "[audio][loop][analysis]") {
    Buffer<float> source(1, 192);
    fill_periodic_sine(source, 64);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    LoopPointAnalysisConfig config;
    config.start_hint_frame = 2;
    config.end_hint_frame = 66;
    config.search_radius_frames = 4;
    config.window_frames = 16;
    config.min_loop_frames = 32;
    config.source_sample_rate = 48000.0;
    config.max_candidates = 4;
    config.snap_policy = LoopSnapPolicy::SlopeMatchedZeroCrossing;

    LoopPointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, config);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.candidates.empty());

    const auto& best = result.candidates.front();
    REQUIRE(best.region.start_frame == 0);
    REQUIRE(best.region.end_frame == 64);
    REQUIRE(best.zero_crossing_cost < 1.0e-5);
}

TEST_CASE("LoopPointAnalyzer snaps hinted endpoints by policy",
          "[audio][loop][analysis]") {
    Buffer<float> source(1, 160);
    fill_periodic_sine(source, 64);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    const auto nearest_zero = LoopPointAnalyzer::snap_endpoint(
        view,
        30,
        LoopSnapPolicy::NearestZeroCrossing,
        4);
    REQUIRE(nearest_zero.valid);
    REQUIRE(nearest_zero.frame == 32);
    REQUIRE(nearest_zero.zero_crossing_cost < 1.0e-6);

    const auto slope_matched = LoopPointAnalyzer::snap_endpoint(
        view,
        34,
        LoopSnapPolicy::SlopeMatchedZeroCrossing,
        34,
        0);
    REQUIRE(slope_matched.valid);
    REQUIRE(slope_matched.frame == 64);
    REQUIRE(slope_matched.zero_crossing_cost < 1.0e-6);

    const auto value_direction = LoopPointAnalyzer::snap_endpoint(
        view,
        70,
        LoopSnapPolicy::ValueDirection,
        6,
        8);
    REQUIRE(value_direction.valid);
    REQUIRE(value_direction.frame == 72);
    REQUIRE(value_direction.value_delta < 1.0e-6);
    REQUIRE(value_direction.slope_delta < 1.0e-6);

    const auto clamped = LoopPointAnalyzer::snap_endpoint(
        view,
        999,
        LoopSnapPolicy::None,
        0);
    REQUIRE(clamped.valid);
    REQUIRE(clamped.frame == 159);
    REQUIRE(clamped.scored_frame == 159);

    const auto exclusive_end = LoopPointAnalyzer::snap_endpoint(
        view,
        999,
        LoopSnapPolicy::None,
        0,
        std::numeric_limits<std::uint64_t>::max(),
        LoopEndpointRole::ExclusiveEndFrame);
    REQUIRE(exclusive_end.valid);
    REQUIRE(exclusive_end.frame == 160);
    REQUIRE(exclusive_end.scored_frame == 159);
}

TEST_CASE("LoopPointAnalyzer does not treat discontinuities as free zero crossings",
          "[audio][loop][analysis]") {
    Buffer<float> full_scale_flip(1, 4);
    full_scale_flip.channel(0)[0] = -1.0f;
    full_scale_flip.channel(0)[1] = 1.0f;
    full_scale_flip.channel(0)[2] = -1.0f;
    full_scale_flip.channel(0)[3] = 1.0f;
    std::vector<const float*> ptrs;
    auto view = const_view(full_scale_flip, ptrs);

    const auto sign_change = LoopPointAnalyzer::snap_endpoint(
        view,
        1,
        LoopSnapPolicy::NearestZeroCrossing,
        1);
    REQUIRE(sign_change.valid);
    REQUIRE(sign_change.zero_crossing_cost > 0.9);
    REQUIRE(sign_change.cost > 0.9);

    Buffer<float> anti_phase(2, 4);
    for (std::size_t i = 0; i < anti_phase.num_samples(); ++i) {
        anti_phase.channel(0)[i] = 1.0f;
        anti_phase.channel(1)[i] = -1.0f;
    }
    view = const_view(anti_phase, ptrs);

    const auto cancelled_stereo = LoopPointAnalyzer::snap_endpoint(
        view,
        2,
        LoopSnapPolicy::NearestZeroCrossing,
        2);
    REQUIRE(cancelled_stereo.valid);
    REQUIRE(cancelled_stereo.zero_crossing_cost > 0.9);
    REQUIRE(cancelled_stereo.cost > 0.9);
}

TEST_CASE("LoopPointAnalyzer consumes slice markers as loop anchors",
          "[audio][loop][analysis][slice]") {
    Buffer<float> source(1, 192);
    fill_periodic_sine(source, 64);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<SliceMarker> anchors = {
        {0, 1.0, SliceMarkerSource::LoopStart},
        {64, 1.0, SliceMarkerSource::LoopEnd},
    };

    LoopPointAnalysisConfig config;
    config.start_hint_frame = 17;
    config.end_hint_frame = 89;
    config.search_radius_frames = 0;
    config.window_frames = 16;
    config.min_loop_frames = 32;
    config.source_sample_rate = 48000.0;
    config.max_candidates = 4;
    config.anchor_markers = std::span<const SliceMarker>(anchors.data(), anchors.size());

    LoopPointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, config);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.candidates.empty());
    REQUIRE(result.candidates.front().region.start_frame == 0);
    REQUIRE(result.candidates.front().region.end_frame == 64);
}

TEST_CASE("LoopPointAnalyzer handles too-short and empty material deterministically",
          "[audio][loop][analysis]") {
    LoopPointAnalyzer analyzer;

    Buffer<float> short_source(1, 8);
    std::vector<const float*> ptrs;
    auto view = const_view(short_source, ptrs);

    LoopPointAnalysisConfig config;
    config.start_hint_frame = 0;
    config.end_hint_frame = 8;
    config.search_radius_frames = 2;
    config.window_frames = 4;
    config.min_loop_frames = 16;
    config.source_sample_rate = 48000.0;
    const auto too_short = analyzer.analyze(view, config);
    REQUIRE_FALSE(too_short.ok);
    REQUIRE(too_short.candidates.empty());
    REQUIRE(too_short.evaluated_candidates == 0);

    Buffer<float> empty(1, 0);
    view = const_view(empty, ptrs);
    const auto no_audio = analyzer.analyze(view, config);
    REQUIRE_FALSE(no_audio.ok);
    REQUIRE(no_audio.candidates.empty());
}
