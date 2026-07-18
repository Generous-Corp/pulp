#include "sampler_api.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace pulp;

static_assert(static_cast<std::uint8_t>(audio::SampleInterpolationPolicy::Hold) == 0);
static_assert(static_cast<std::uint8_t>(audio::SampleInterpolationPolicy::Nearest) == 1);
static_assert(static_cast<std::uint8_t>(audio::SampleInterpolationPolicy::Linear) == 2);
static_assert(static_cast<std::uint8_t>(audio::SampleInterpolationPolicy::CubicHermite) == 3);
static_assert(static_cast<std::uint8_t>(audio::SampleInterpolationPolicy::CubicLagrange) == 4);
static_assert(static_cast<std::uint8_t>(audio::SampleInterpolationPolicy::RatioTrackingSinc) == 5);
static_assert(audio::kSampleInterpolationPolicies.size() == 6);
static_assert(audio::sample_interpolation_policy_id(
                  audio::SampleInterpolationPolicy::Hold) == "hold");
static_assert(audio::sample_interpolation_policy_name(
                  audio::SampleInterpolationPolicy::RatioTrackingSinc) ==
              "Ratio-tracking sinc");

TEST_CASE("Sample interpolation policies expose stable ids and names",
          "[audio][sampler][api]") {
    constexpr std::string_view ids[]{
        "hold", "nearest", "linear", "cubic-hermite", "cubic-lagrange",
        "ratio-tracking-sinc"};
    constexpr std::string_view names[]{
        "Hold", "Nearest", "Linear", "Cubic Hermite", "Cubic Lagrange",
        "Ratio-tracking sinc"};

    REQUIRE(audio::kSampleInterpolationPolicies.size() == 6);
    for (std::size_t index = 0; index < audio::kSampleInterpolationPolicies.size();
         ++index) {
        const auto policy = static_cast<audio::SampleInterpolationPolicy>(index);
        REQUIRE(audio::sample_interpolation_policy_id(policy) == ids[index]);
        REQUIRE(audio::sample_interpolation_policy_name(policy) == names[index]);
        const auto* by_id = audio::sample_interpolation_policy_info(ids[index]);
        REQUIRE(by_id != nullptr);
        REQUIRE(by_id->policy == policy);
    }

    const auto invalid = static_cast<audio::SampleInterpolationPolicy>(255);
    REQUIRE(audio::sample_interpolation_policy_id(invalid).empty());
    REQUIRE(audio::sample_interpolation_policy_name(invalid) == "Unknown");
    REQUIRE(audio::sample_interpolation_policy_info("not-a-policy") == nullptr);
}

TEST_CASE("PulpSampler public API records have stable defaults and status names",
          "[audio][sampler][api]") {
    examples::PulpSamplerConfig config;
    REQUIRE(config.streaming_memory_budget_bytes == 0);

    examples::PulpSamplerDiagnostics diagnostics;
    REQUIRE_FALSE(diagnostics.prepare.prepared());
    REQUIRE(diagnostics.prepare.status ==
            examples::PulpSamplerPrepareStatus::NotPrepared);
    REQUIRE(examples::pulp_sampler_prepare_status_name(
                diagnostics.prepare.status) == "not-prepared");
    REQUIRE_FALSE(diagnostics.last_load.loaded());
    REQUIRE(examples::pulp_sampler_load_status_name(
                diagnostics.last_load.status) == "not-attempted");
    REQUIRE(examples::pulp_sampler_codec_capability_name(
                diagnostics.last_load.codec_capability) == "unknown");
    REQUIRE(examples::pulp_sampler_sidecar_status_name(
                diagnostics.last_load.sidecar_status) == "not-present");
    REQUIRE(diagnostics.preload.maximum_playback_ratio == 4.0);
    REQUIRE(diagnostics.preload.certified_io_latency_seconds == 0.020);
    REQUIRE_FALSE(diagnostics.preload.sufficient());
    REQUIRE(diagnostics.heritage.status ==
            examples::PulpSamplerHeritageStatus::Disabled);
    REQUIRE(examples::pulp_sampler_heritage_status_name(
                diagnostics.heritage.status) == "disabled");
    REQUIRE(diagnostics.heritage.clock_ratio == 1.0);
    REQUIRE(diagnostics.heritage.all_stages_bypassed);
}

TEST_CASE("PulpSampler public result helpers expose bounded fixed names",
          "[audio][sampler][api]") {
    examples::PulpSamplerLoadResult load;
    constexpr std::string_view codec = "WAV";
    std::copy(codec.begin(), codec.end(), load.codec_name.begin());
    load.status = examples::PulpSamplerLoadStatus::Ok;
    load.codec_capability = examples::PulpSamplerCodecCapability::Ranged;
    REQUIRE(load.loaded());
    REQUIRE(load.codec() == codec);

    examples::PulpSamplerHeritageDiagnostics heritage;
    constexpr std::string_view profile = "neutral.synthetic-v1";
    std::copy(profile.begin(), profile.end(), heritage.profile_id.begin());
    REQUIRE(heritage.profile() == profile);
}
