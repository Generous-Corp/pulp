#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/sampler_looper_metrics.hpp>
#include <pulp/audio/slice_map.hpp>
#include <pulp/audio/slice_point_analyzer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::OnsetDetectionConfig;
using pulp::audio::OnsetDetectionMethod;
using pulp::audio::OnsetDetector;
using pulp::audio::OnsetMarker;
using pulp::audio::SliceMarkerSource;
using pulp::audio::SlicePointAnalysisConfig;
using pulp::audio::SlicePointAnalyzer;
using pulp::audio::validate_slice_map;

namespace {

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

bool has_marker_near(const std::vector<OnsetMarker>& markers,
                     std::uint64_t frame,
                     std::uint64_t tolerance) {
    for (const auto& marker : markers) {
        const auto lo = frame > tolerance ? frame - tolerance : 0;
        const auto hi = frame + tolerance;
        if (marker.frame >= lo && marker.frame <= hi) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("OnsetDetector detects synthetic energy attacks",
          "[audio][onset][slice]") {
    Buffer<float> source(1, 4096);
    source.channel(0)[1024] = 1.0f;
    source.channel(0)[2048] = 0.8f;
    std::vector<const float*> ptrs;

    OnsetDetectionConfig config;
    config.method = OnsetDetectionMethod::EnergyFlux;
    config.frame_size = 128;
    config.hop_size = 64;
    config.adaptive_window_frames = 4;
    config.min_spacing_frames = 512;
    config.threshold_multiplier = 1.2;
    config.min_confidence = 0.05;

    OnsetDetector detector;
    const auto result = detector.detect(const_view(source, ptrs), config);
    REQUIRE(result.ok);
    REQUIRE(result.analyzed_frames == source.num_samples());
    REQUIRE(result.markers.size() >= 2);
    REQUIRE(has_marker_near(result.markers, 1024, 128));
    REQUIRE(has_marker_near(result.markers, 2048, 128));

    const auto metrics =
        pulp::audio::collect_sampler_looper_metrics(nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    &result,
                                                    nullptr);
    REQUIRE(metrics.onset_count == result.markers.size());
}

TEST_CASE("OnsetDetector returns no markers for silence",
          "[audio][onset][slice]") {
    Buffer<float> source(2, 2048);
    std::vector<const float*> ptrs;

    OnsetDetectionConfig config;
    config.frame_size = 128;
    config.hop_size = 64;

    OnsetDetector detector;
    const auto result = detector.detect(const_view(source, ptrs), config);
    REQUIRE(result.ok);
    REQUIRE(result.markers.empty());
}

TEST_CASE("OnsetDetector spectral modes require power-of-two frames",
          "[audio][onset][slice]") {
    Buffer<float> source(1, 1024);
    source.channel(0)[512] = 1.0f;
    std::vector<const float*> ptrs;

    OnsetDetectionConfig config;
    config.method = OnsetDetectionMethod::SpectralFlux;
    config.frame_size = 192;
    config.hop_size = 64;

    OnsetDetector detector;
    const auto result = detector.detect(const_view(source, ptrs), config);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.markers.empty());
}

TEST_CASE("OnsetDetector detects attacks with FFT-backed spectral flux",
          "[audio][onset][slice]") {
    Buffer<float> source(1, 4096);
    source.channel(0)[1024] = 1.0f;
    source.channel(0)[2048] = -0.8f;
    std::vector<const float*> ptrs;

    OnsetDetectionConfig config;
    config.method = OnsetDetectionMethod::SpectralFlux;
    config.frame_size = 256;
    config.hop_size = 64;
    config.adaptive_window_frames = 4;
    config.min_spacing_frames = 512;
    config.threshold_multiplier = 1.2;
    config.min_confidence = 0.05;

    OnsetDetector detector;
    const auto result = detector.detect(const_view(source, ptrs), config);
    REQUIRE(result.ok);
    REQUIRE(result.analyzed_frames == source.num_samples());
    REQUIRE(result.markers.size() >= 2);
    REQUIRE(has_marker_near(result.markers, 1024, 256));
    REQUIRE(has_marker_near(result.markers, 2048, 256));
}

TEST_CASE("SlicePointAnalyzer builds ordered regions from debounced onsets",
          "[audio][onset][slice]") {
    Buffer<float> source(1, 4096);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {1000, 0.6, OnsetDetectionMethod::EnergyFlux},
        {1050, 0.9, OnsetDetectionMethod::EnergyFlux},
        {2000, 0.7, OnsetDetectionMethod::EnergyFlux},
    };

    SlicePointAnalysisConfig config;
    config.source_generation = 17;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 256;
    config.snap_to_zero_crossing = false;
    std::vector<pulp::audio::SliceMarker> additional = {
        {3000, 0.5, SliceMarkerSource::BeatGrid},
        {3500, 1.0, SliceMarkerSource::LoopStart},
    };
    config.additional_markers =
        std::span<const pulp::audio::SliceMarker>(additional.data(), additional.size());

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.map.source_generation == 17);
    REQUIRE(result.map.markers.size() == 5);
    REQUIRE(result.map.markers[0].frame == 0);
    REQUIRE(result.map.markers[1].frame == 1050);
    REQUIRE(result.map.markers[1].source == SliceMarkerSource::Onset);
    REQUIRE(result.map.markers[3].frame == 3000);
    REQUIRE(result.map.markers[3].source == SliceMarkerSource::BeatGrid);
    REQUIRE(result.map.markers[4].frame == 3500);
    REQUIRE(result.map.markers[4].source == SliceMarkerSource::LoopStart);
    REQUIRE(result.map.regions.size() == 5);
    REQUIRE(result.map.regions[0].start_frame == 0);
    REQUIRE(result.map.regions[0].end_frame == 1050);
    REQUIRE(result.map.regions[4].end_frame == source.num_samples());
}

TEST_CASE("SlicePointAnalyzer sorts and prioritizes snapped markers",
          "[audio][onset][slice]") {
    Buffer<float> source(1, 2048);
    std::fill(source.channel(0).begin(), source.channel(0).end(), 1.0f);
    source.channel(0)[1000] = 0.0f;
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {990, 1.0, OnsetDetectionMethod::EnergyFlux},
    };
    std::vector<pulp::audio::SliceMarker> additional = {
        {1010, 1.0, SliceMarkerSource::LoopStart},
    };

    SlicePointAnalysisConfig config;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 64;
    config.snap_radius_frames = 16;
    config.snap_to_zero_crossing = true;
    config.additional_markers =
        std::span<const pulp::audio::SliceMarker>(additional.data(), additional.size());

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.map.markers.size() == 2);
    REQUIRE(result.map.markers[0].frame == 0);
    REQUIRE(result.map.markers[1].frame == 1000);
    REQUIRE(result.map.markers[1].source == SliceMarkerSource::LoopStart);
    REQUIRE(result.map.regions[0].start_frame == 0);
    REQUIRE(result.map.regions[0].end_frame == 1000);
}

TEST_CASE("SlicePointAnalyzer supports 60-second slice maps",
          "[audio][onset][slice]") {
    const auto frames_60s = static_cast<std::uint64_t>(48000 * 60);
    Buffer<float> source(1, static_cast<std::size_t>(frames_60s));
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {48000, 0.8, OnsetDetectionMethod::EnergyFlux},
        {48000 * 30, 0.9, OnsetDetectionMethod::EnergyFlux},
        {48000 * 59, 0.7, OnsetDetectionMethod::EnergyFlux},
    };

    SlicePointAnalysisConfig config;
    config.source_generation = 60;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 1024;
    config.snap_to_zero_crossing = false;

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.map.source_frames == frames_60s);
    REQUIRE(result.map.markers.size() == 4);
    REQUIRE(result.map.regions.back().end_frame == frames_60s);

    const auto metrics =
        pulp::audio::collect_sampler_looper_metrics(nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    &result.map);
    REQUIRE(metrics.slice_count == result.map.markers.size());
}
