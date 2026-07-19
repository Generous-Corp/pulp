#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/sampler_looper_metrics.hpp>
#include <pulp/audio/slice_map.hpp>
#include <pulp/audio/slice_point_analyzer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

using pulp::audio::AnalyzerBackend;
using pulp::audio::AnalyzerCapability;
using pulp::audio::AnalyzerMarkerProvenance;
using pulp::audio::AnalyzerProvenance;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::OnsetDetectionConfig;
using pulp::audio::OnsetDetectionMethod;
using pulp::audio::OnsetDetector;
using pulp::audio::OnsetMarker;
using pulp::audio::SliceMarkerSource;
using pulp::audio::SliceCandidateSelection;
using pulp::audio::SlicePointAnalysisConfig;
using pulp::audio::SlicePointAnalyzer;
using pulp::audio::SliceSelectionOptions;
using pulp::audio::SliceSnapPolicy;
using pulp::audio::TransientClass;
using pulp::audio::TransientClassification;
using pulp::audio::kUnmappedTransientCandidateMarkerIndex;
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

AnalyzerProvenance package_provenance(std::string provider_id,
                                      std::string analysis_id) {
    AnalyzerProvenance provenance;
    provenance.provider_id = std::move(provider_id);
    provenance.package_id = "package.test";
    provenance.version = "1.0";
    provenance.analysis_id = std::move(analysis_id);
    provenance.backend = AnalyzerBackend::Package;
    return provenance;
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

TEST_CASE("SlicePointAnalyzer strongest selection is bounded and timeline sorted",
          "[audio][onset][slice][selection]") {
    Buffer<float> source(1, 1000);
    std::vector<const float*> ptrs;
    std::vector<OnsetMarker> onsets = {
        {200, 0.2, OnsetDetectionMethod::EnergyFlux},
        {400, 0.9, OnsetDetectionMethod::EnergyFlux},
        {700, 0.8, OnsetDetectionMethod::EnergyFlux},
        {850, 0.7, OnsetDetectionMethod::EnergyFlux},
    };
    SlicePointAnalysisConfig config;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 100;
    config.snap_to_zero_crossing = false;
    SliceSelectionOptions options;
    options.max_regions = 3;
    options.candidate_selection = SliceCandidateSelection::StrongestConfidence;

    const auto result = SlicePointAnalyzer{}.analyze(
        const_view(source, ptrs), onsets, config, options);
    REQUIRE(result.ok);
    REQUIRE(result.map.markers.size() == 3);
    CHECK(result.map.markers[0].frame == 0);
    CHECK(result.map.markers[1].frame == 400);
    CHECK(result.map.markers[2].frame == 700);
}

TEST_CASE("SlicePointAnalyzer strongest ties and spacing guards are deterministic",
          "[audio][onset][slice][selection]") {
    Buffer<float> source(1, 1000);
    std::vector<const float*> ptrs;
    std::vector<OnsetMarker> onsets = {
        {50, 1.0, OnsetDetectionMethod::EnergyFlux},
        {300, 0.8, OnsetDetectionMethod::EnergyFlux},
        {700, 0.8, OnsetDetectionMethod::EnergyFlux},
        {950, 1.0, OnsetDetectionMethod::EnergyFlux},
    };
    SlicePointAnalysisConfig config;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 100;
    config.snap_to_zero_crossing = false;
    SliceSelectionOptions options;
    options.max_regions = 2;
    options.candidate_selection = SliceCandidateSelection::StrongestConfidence;

    const auto result = SlicePointAnalyzer{}.analyze(
        const_view(source, ptrs), onsets, config, options);
    REQUIRE(result.ok);
    REQUIRE(result.map.markers.size() == 2);
    CHECK(result.map.markers[1].frame == 300);
    CHECK(result.map.regions.back().end_frame == 1000);
}

TEST_CASE("SlicePointAnalyzer supports sign-transition snapping",
          "[audio][onset][slice][snap]") {
    Buffer<float> source(1, 100);
    std::fill(source.channel(0).begin(), source.channel(0).end(), 1.0f);
    std::fill(source.channel(0).begin() + 50, source.channel(0).end(), -1.0f);
    std::vector<const float*> ptrs;
    const std::vector<OnsetMarker> onsets = {
        {53, 1.0, OnsetDetectionMethod::EnergyFlux},
    };
    SlicePointAnalysisConfig config;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 10;
    config.snap_radius_frames = 5;

    SliceSelectionOptions near_zero;
    const auto unchanged = SlicePointAnalyzer{}.analyze(
        const_view(source, ptrs), onsets, config, near_zero);
    REQUIRE(unchanged.ok);
    CHECK(unchanged.map.markers[1].frame == 53);

    SliceSelectionOptions transition;
    transition.snap_policy = SliceSnapPolicy::SignTransition;
    const auto snapped = SlicePointAnalyzer{}.analyze(
        const_view(source, ptrs), onsets, config, transition);
    REQUIRE(snapped.ok);
    CHECK(snapped.map.markers[1].frame == 50);
}

TEST_CASE("SlicePointAnalyzer remaps analyzer provenance through debounced markers",
          "[audio][onset][slice][provider]") {
    Buffer<float> source(1, 4096);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {1000, 0.5, OnsetDetectionMethod::EnergyFlux},
        {1100, 0.9, OnsetDetectionMethod::EnergyFlux},
        {2400, 0.7, OnsetDetectionMethod::SpectralFlux},
    };
    std::vector<AnalyzerMarkerProvenance> onset_provenance = {
        {0, AnalyzerCapability::OnsetDetection,
         package_provenance("package.test.onset", "candidate-a")},
        {1, AnalyzerCapability::OnsetDetection,
         package_provenance("package.test.onset", "candidate-b")},
        {2, AnalyzerCapability::OnsetDetection,
         package_provenance("package.test.onset", "candidate-c")},
    };

    SlicePointAnalysisConfig config;
    config.source_generation = 19;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 256;
    config.snap_to_zero_crossing = false;
    config.onset_provenance = std::span<const AnalyzerMarkerProvenance>(
        onset_provenance.data(), onset_provenance.size());

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.map.markers.size() == 3);
    REQUIRE(result.map.markers[1].frame == 1100);
    REQUIRE(result.map.markers[2].frame == 2400);
    REQUIRE(result.marker_provenance.size() == 2);
    REQUIRE(result.marker_provenance[0].marker_index == 1);
    REQUIRE(result.marker_provenance[0].provenance.analysis_id == "candidate-b");
    REQUIRE(result.marker_provenance[1].marker_index == 2);
    REQUIRE(result.marker_provenance[1].provenance.analysis_id == "candidate-c");
}

TEST_CASE("SlicePointAnalyzer attaches transient classifications to nearest markers",
          "[audio][onset][slice][provider]") {
    Buffer<float> source(1, 4096);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {1000, 0.8, OnsetDetectionMethod::EnergyFlux},
        {2500, 0.7, OnsetDetectionMethod::EnergyFlux},
    };
    std::vector<TransientClassification> classifications = {
        {1008, 0.70, TransientClass::Kick,
         package_provenance("package.test.transient", "kick-low")},
        {1012, 0.95, TransientClass::Snare,
         package_provenance("package.test.transient", "snare-high")},
        {2600, 0.80, TransientClass::Hat,
         package_provenance("package.test.transient", "hat")},
        {3600, 0.99, TransientClass::Vocal,
         package_provenance("package.test.transient", "too-far")},
    };

    SlicePointAnalysisConfig config;
    config.source_generation = 20;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 256;
    config.snap_to_zero_crossing = false;
    config.transient_classifications = std::span<const TransientClassification>(
        classifications.data(), classifications.size());
    config.transient_match_radius_frames = 128;

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.marker_classifications.size() == 2);
    REQUIRE(result.marker_classifications[0].marker_index == 1);
    REQUIRE(result.marker_classifications[0].transient_class == TransientClass::Snare);
    REQUIRE(result.marker_classifications[0].confidence == 0.95);
    REQUIRE(result.marker_classifications[0].provenance.analysis_id == "snare-high");
    REQUIRE(result.marker_classifications[1].marker_index == 2);
    REQUIRE(result.marker_classifications[1].transient_class == TransientClass::Hat);
}

TEST_CASE("SlicePointAnalyzer maps transient classifications by candidate identity",
          "[audio][onset][slice][provider]") {
    Buffer<float> source(1, 4096);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {1000, 0.8, OnsetDetectionMethod::EnergyFlux},
        {2500, 0.7, OnsetDetectionMethod::EnergyFlux},
    };
    auto provenance = package_provenance("package.test.transient", "identity-map");
    std::vector<TransientClassification> classifications(5);
    classifications[0].frame = 1000;
    classifications[0].confidence = 0.70;
    classifications[0].transient_class = TransientClass::Kick;
    classifications[0].provenance = provenance;
    classifications[0].candidate_index = 0;
    classifications[0].has_candidate_index = true;
    classifications[1].frame = 1000;
    classifications[1].confidence = 0.80;
    classifications[1].transient_class = TransientClass::Hat;
    classifications[1].provenance = provenance;
    classifications[1].candidate_index = 1;
    classifications[1].has_candidate_index = true;
    classifications[2].frame = 2500;
    classifications[2].confidence = 0.99;
    classifications[2].transient_class = TransientClass::Vocal;
    classifications[2].provenance = provenance;
    classifications[2].candidate_index = 2;
    classifications[2].has_candidate_index = true;
    classifications[3].frame = 1000;
    classifications[3].confidence = 0.98;
    classifications[3].transient_class = TransientClass::Snare;
    classifications[3].provenance = provenance;
    classifications[3].candidate_index = 3;
    classifications[3].has_candidate_index = true;
    classifications[4].frame = 2500;
    classifications[4].confidence = 0.97;
    classifications[4].transient_class = TransientClass::Clap;
    classifications[4].provenance = provenance;
    classifications[4].candidate_index = 99;
    classifications[4].has_candidate_index = true;
    std::vector<std::uint32_t> candidate_marker_indices = {
        1,
        2,
        kUnmappedTransientCandidateMarkerIndex,
        99,
    };

    SlicePointAnalysisConfig config;
    config.source_generation = 21;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 256;
    config.snap_to_zero_crossing = false;
    config.transient_classifications = std::span<const TransientClassification>(
        classifications.data(), classifications.size());
    config.transient_candidate_marker_indices = std::span<const std::uint32_t>(
        candidate_marker_indices.data(), candidate_marker_indices.size());
    config.transient_match_radius_frames = 32;

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.marker_classifications.size() == 2);
    REQUIRE(result.marker_classifications[0].marker_index == 1);
    REQUIRE(result.marker_classifications[0].transient_class == TransientClass::Kick);
    REQUIRE(result.marker_classifications[1].marker_index == 2);
    REQUIRE(result.marker_classifications[1].transient_class == TransientClass::Hat);
}

TEST_CASE("SlicePointAnalyzer nearest-matches transient classifications without candidate identity",
          "[audio][onset][slice][provider]") {
    Buffer<float> source(1, 2048);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {1000, 0.8, OnsetDetectionMethod::EnergyFlux},
    };
    std::vector<TransientClassification> classifications = {
        {1008, 0.70, TransientClass::Kick,
         package_provenance("package.test.transient", "no-candidate-id")},
    };
    std::vector<std::uint32_t> candidate_marker_indices = {1};

    SlicePointAnalysisConfig config;
    config.source_generation = 22;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 256;
    config.snap_to_zero_crossing = false;
    config.transient_classifications = std::span<const TransientClassification>(
        classifications.data(), classifications.size());
    config.transient_candidate_marker_indices = std::span<const std::uint32_t>(
        candidate_marker_indices.data(), candidate_marker_indices.size());
    config.transient_match_radius_frames = 32;

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.marker_classifications.size() == 1);
    REQUIRE(result.marker_classifications[0].marker_index == 1);
    REQUIRE(result.marker_classifications[0].transient_class == TransientClass::Kick);
}

TEST_CASE("SlicePointAnalyzer ignores invalid marker confidences before sorting",
          "[audio][onset][slice][provider]") {
    Buffer<float> source(1, 2048);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {512, std::numeric_limits<double>::quiet_NaN(), OnsetDetectionMethod::EnergyFlux},
        {1000, 0.8, OnsetDetectionMethod::EnergyFlux},
    };
    std::vector<pulp::audio::SliceMarker> additional = {
        {1500, -0.1, SliceMarkerSource::Imported},
    };

    SlicePointAnalysisConfig config;
    config.source_sample_rate = 48000.0;
    config.min_slice_frames = 128;
    config.snap_to_zero_crossing = false;
    config.additional_markers =
        std::span<const pulp::audio::SliceMarker>(additional.data(), additional.size());

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE(result.ok);
    REQUIRE(validate_slice_map(result.map));
    REQUIRE(result.map.markers.size() == 2);
    REQUIRE(result.map.markers[1].frame == 1000);
}

TEST_CASE("SlicePointAnalyzer rejects malformed onset provenance sidecars",
          "[audio][onset][slice][provider]") {
    Buffer<float> source(1, 2048);
    std::vector<const float*> ptrs;
    const auto view = const_view(source, ptrs);

    std::vector<OnsetMarker> onsets = {
        {1000, 0.8, OnsetDetectionMethod::EnergyFlux},
    };
    std::vector<AnalyzerMarkerProvenance> bad_provenance = {
        {1, AnalyzerCapability::OnsetDetection,
         package_provenance("package.test.onset", "out-of-range")},
    };

    SlicePointAnalysisConfig config;
    config.source_sample_rate = 48000.0;
    config.onset_provenance = std::span<const AnalyzerMarkerProvenance>(
        bad_provenance.data(), bad_provenance.size());

    SlicePointAnalyzer analyzer;
    const auto result = analyzer.analyze(view, onsets, config);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.map.markers.empty());
    REQUIRE(result.marker_provenance.empty());
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
