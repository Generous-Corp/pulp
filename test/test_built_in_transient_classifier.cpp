#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/built_in_transient_classifier.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <cstdint>
#include <span>
#include <vector>

using pulp::audio::AnalyzerAvailability;
using pulp::audio::AnalyzerBackend;
using pulp::audio::AnalyzerCapability;
using pulp::audio::AnalyzerExecutionContext;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::BuiltInTransientClassifier;
using pulp::audio::TransientClass;
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

void add_decay(Buffer<float>& buffer,
               std::size_t start,
               double frequency_cycles_per_sample,
               double decay,
               double gain) {
    constexpr double two_pi = 6.28318530717958647692;
    auto channel = buffer.channel(0);
    for (std::size_t i = 0; i < 512 && start + i < channel.size(); ++i) {
        const auto envelope = std::exp(-static_cast<double>(i) / decay);
        const auto value = gain * envelope * std::sin(two_pi * frequency_cycles_per_sample * i);
        channel[start + i] += static_cast<float>(value);
    }
}

void add_noise(Buffer<float>& buffer, std::size_t start, double gain) {
    auto channel = buffer.channel(0);
    for (std::size_t i = 0; i < 256 && start + i < channel.size(); ++i) {
        const auto sign = (i * 1103515245u + 12345u) & 0x1u ? 1.0 : -1.0;
        channel[start + i] += static_cast<float>(gain * sign);
    }
}

}  // namespace

TEST_CASE("BuiltInTransientClassifier exposes a package-free fallback descriptor",
          "[audio][analysis][transient]") {
    BuiltInTransientClassifier classifier;
    const auto& descriptor = classifier.descriptor();

    REQUIRE(descriptor.id == "builtin.transient-classifier");
    REQUIRE(descriptor.backend == AnalyzerBackend::BuiltIn);
    REQUIRE(descriptor.availability == AnalyzerAvailability::Available);
    REQUIRE(descriptor.execution_context == AnalyzerExecutionContext::BackgroundThread);
    REQUIRE(descriptor.is_fallback);
    REQUIRE(descriptor.supports_offline_buffers);
    REQUIRE_FALSE(descriptor.supports_streaming_input);
    REQUIRE(analyzer_descriptor_has_capability(descriptor,
                                               AnalyzerCapability::TransientClassification));

    const auto builtins = built_in_analyzer_descriptors();
    const auto found = std::find_if(builtins.begin(), builtins.end(), [](const auto& item) {
        return item.id == "builtin.transient-classifier";
    });
    REQUIRE(found != builtins.end());
    REQUIRE(found->display_name == descriptor.display_name);
    REQUIRE(found->capabilities == descriptor.capabilities);
    REQUIRE(found->is_fallback == descriptor.is_fallback);
}

TEST_CASE("BuiltInTransientClassifier classifies simple low and high transients",
          "[audio][analysis][transient]") {
    Buffer<float> source(1, 4096);
    add_decay(source, 1024, 2.0 / 1024.0, 180.0, 1.0);
    add_noise(source, 2600, 0.4);

    std::vector<std::uint64_t> candidates = {1024, 2600};
    std::vector<const float*> ptrs;

    BuiltInTransientClassifier classifier;
    const auto result = classifier.classify(
        const_view(source, ptrs),
        std::span<const std::uint64_t>(candidates.data(), candidates.size()));

    REQUIRE(result.size() == 2);
    REQUIRE(result[0].frame == 1024);
    REQUIRE(result[0].transient_class == TransientClass::Kick);
    REQUIRE(result[0].confidence > 0.0);
    REQUIRE(result[0].provenance.provider_id == classifier.descriptor().id);
    REQUIRE(result[0].has_candidate_index);
    REQUIRE(result[0].candidate_index == 0);
    REQUIRE(result[1].frame == 2600);
    REQUIRE((result[1].transient_class == TransientClass::Hat ||
             result[1].transient_class == TransientClass::Noise));
    REQUIRE(result[1].confidence > 0.0);
    REQUIRE(result[1].has_candidate_index);
    REQUIRE(result[1].candidate_index == 1);
}

TEST_CASE("BuiltInTransientClassifier skips invalid or silent candidates",
          "[audio][analysis][transient]") {
    Buffer<float> source(1, 4096);
    add_decay(source, 2048, 2.0 / 1024.0, 180.0, 1.0);
    std::vector<std::uint64_t> candidates = {256, 2048, 8192};
    std::vector<const float*> ptrs;

    BuiltInTransientClassifier classifier;
    const auto result = classifier.classify(
        const_view(source, ptrs),
        std::span<const std::uint64_t>(candidates.data(), candidates.size()));

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].frame == 2048);
    REQUIRE(result[0].has_candidate_index);
    REQUIRE(result[0].candidate_index == 1);
}

TEST_CASE("BuiltInTransientClassifier suppresses zero-evidence classifications",
          "[audio][analysis][transient]") {
    Buffer<float> source(1, 2048);
    source.channel(0)[0] = 1.0f;
    auto channel = source.channel(0);
    for (std::size_t i = 256; i < 768; ++i) channel[i] += 0.25f;
    channel[1024] = std::numeric_limits<float>::quiet_NaN();
    std::vector<std::uint64_t> candidates = {0, 512, 1024};
    std::vector<const float*> ptrs;

    BuiltInTransientClassifier classifier;
    const auto result = classifier.classify(
        const_view(source, ptrs),
        std::span<const std::uint64_t>(candidates.data(), candidates.size()));

    for (const auto& item : result) {
        REQUIRE(item.confidence > 0.0);
        REQUIRE(std::isfinite(item.confidence));
        REQUIRE(item.transient_class != TransientClass::Unknown);
    }
}

TEST_CASE("BuiltInTransientClassifier returns no classifications for malformed input",
          "[audio][analysis][transient]") {
    Buffer<float> source(1, 1024);
    std::vector<const float*> ptrs;
    auto view = const_view(source, ptrs);
    ptrs[0] = nullptr;
    view = {ptrs.data(), 1, source.num_samples()};

    std::vector<std::uint64_t> candidates = {128};
    BuiltInTransientClassifier classifier;
    const auto result = classifier.classify(
        view,
        std::span<const std::uint64_t>(candidates.data(), candidates.size()));

    REQUIRE(result.empty());
}
