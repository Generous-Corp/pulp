#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/built_in_key_tempo_analyzer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using pulp::audio::AnalyzerAvailability;
using pulp::audio::AnalyzerBackend;
using pulp::audio::AnalyzerCapability;
using pulp::audio::AnalyzerExecutionContext;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::BuiltInKeyTempoAnalyzer;
using pulp::audio::KeyTempoAnalysisConfig;
using pulp::audio::MusicalKeyMode;
using pulp::audio::analyzer_descriptor_has_capability;
using pulp::audio::built_in_analyzer_descriptors;

namespace {

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

void add_sine(Buffer<float>& buffer,
              double sample_rate,
              double frequency_hz,
              double gain) {
    constexpr double two_pi = 6.28318530717958647692;
    auto channel = buffer.channel(0);
    for (std::size_t i = 0; i < channel.size(); ++i) {
        const auto phase = two_pi * frequency_hz * static_cast<double>(i) / sample_rate;
        channel[i] += static_cast<float>(gain * std::sin(phase));
    }
}

}  // namespace

TEST_CASE("BuiltInKeyTempoAnalyzer exposes a package-free descriptor",
          "[audio][analysis][key-tempo]") {
    BuiltInKeyTempoAnalyzer analyzer;
    const auto& descriptor = analyzer.descriptor();

    REQUIRE(descriptor.id == "builtin.key-tempo-analyzer");
    REQUIRE(descriptor.backend == AnalyzerBackend::BuiltIn);
    REQUIRE(descriptor.availability == AnalyzerAvailability::Available);
    REQUIRE(descriptor.execution_context == AnalyzerExecutionContext::BackgroundThread);
    REQUIRE(descriptor.is_fallback);
    REQUIRE(descriptor.supports_offline_buffers);
    REQUIRE_FALSE(descriptor.supports_streaming_input);
    REQUIRE(analyzer_descriptor_has_capability(descriptor, AnalyzerCapability::KeyDetection));
    REQUIRE(analyzer_descriptor_has_capability(descriptor, AnalyzerCapability::TempoDetection));

    const auto builtins = built_in_analyzer_descriptors();
    const auto found = std::find_if(builtins.begin(), builtins.end(), [](const auto& item) {
        return item.id == "builtin.key-tempo-analyzer";
    });
    REQUIRE(found != builtins.end());
    REQUIRE(found->display_name == descriptor.display_name);
    REQUIRE(found->capabilities == descriptor.capabilities);
    REQUIRE(found->is_fallback == descriptor.is_fallback);
}

TEST_CASE("BuiltInKeyTempoAnalyzer estimates tempo from synthetic attacks",
          "[audio][analysis][key-tempo]") {
    constexpr double sample_rate = 48000.0;
    Buffer<float> source(1, static_cast<std::size_t>(sample_rate * 4.0));
    for (std::size_t frame = 0; frame < source.num_samples(); frame += 24000) {
        source.channel(0)[frame] = 1.0f;
    }
    std::vector<const float*> ptrs;

    KeyTempoAnalysisConfig config;
    config.source_sample_rate = sample_rate;
    config.channels = 1;
    config.estimate_key = false;
    config.estimate_tempo = true;

    BuiltInKeyTempoAnalyzer analyzer;
    const auto result = analyzer.analyze(const_view(source, ptrs), config);
    REQUIRE(result.ok);
    REQUIRE(result.tempo_bpm >= 118.0);
    REQUIRE(result.tempo_bpm <= 122.0);
    REQUIRE(result.tempo_confidence > 0.0);
    REQUIRE(result.key_root == -1);
    REQUIRE(result.provenance.provider_id == analyzer.descriptor().id);
}

TEST_CASE("BuiltInKeyTempoAnalyzer estimates key from a simple C major chord",
          "[audio][analysis][key-tempo]") {
    constexpr double sample_rate = 48000.0;
    Buffer<float> source(1, 16384);
    // Use frequencies aligned to the analyzer's 2048-point FFT bins so the
    // fixture tests pitch-class scoring instead of leakage between bins.
    add_sine(source, sample_rate, sample_rate * 11.0 / 2048.0, 0.3);  // C
    add_sine(source, sample_rate, sample_rate * 14.0 / 2048.0, 0.3);  // E
    add_sine(source, sample_rate, sample_rate * 17.0 / 2048.0, 0.3);  // G
    std::vector<const float*> ptrs;

    KeyTempoAnalysisConfig config;
    config.source_sample_rate = sample_rate;
    config.channels = 1;
    config.estimate_key = true;
    config.estimate_tempo = false;

    BuiltInKeyTempoAnalyzer analyzer;
    const auto result = analyzer.analyze(const_view(source, ptrs), config);
    REQUIRE(result.ok);
    REQUIRE(result.key_root == 0);
    REQUIRE(result.key_mode == MusicalKeyMode::Major);
    REQUIRE(result.key_confidence > 0.0);
    REQUIRE(result.tempo_bpm == 0.0);
}

TEST_CASE("BuiltInKeyTempoAnalyzer rejects invalid analysis configs",
          "[audio][analysis][key-tempo]") {
    Buffer<float> source(1, 2048);
    std::vector<const float*> ptrs;

    KeyTempoAnalysisConfig config;
    config.source_sample_rate = 0.0;
    config.channels = 1;

    BuiltInKeyTempoAnalyzer analyzer;
    const auto result = analyzer.analyze(const_view(source, ptrs), config);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.tempo_bpm == 0.0);
    REQUIRE(result.key_root == -1);
}
