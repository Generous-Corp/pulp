#include "sampler_api.hpp"
#include "sampler_streaming_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace pulp;

namespace {

struct TemporarySamplerApiFiles {
    std::filesystem::path source;

    explicit TemporarySamplerApiFiles(std::filesystem::path source_path)
        : source(std::move(source_path)) {}
    TemporarySamplerApiFiles(TemporarySamplerApiFiles&& other) noexcept
        : source(std::exchange(other.source, {})) {}
    TemporarySamplerApiFiles& operator=(TemporarySamplerApiFiles&&) = delete;
    TemporarySamplerApiFiles(const TemporarySamplerApiFiles&) = delete;
    TemporarySamplerApiFiles& operator=(const TemporarySamplerApiFiles&) = delete;

    ~TemporarySamplerApiFiles() {
        if (source.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(source, ignored);
        std::filesystem::remove(source.string() + ".pulpmip", ignored);
    }
};

TemporarySamplerApiFiles make_sampler_api_wav() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    TemporarySamplerApiFiles files{
        std::filesystem::temp_directory_path() /
        ("pulp-sampler-api-" + std::to_string(nonce) + ".wav")};
    audio::AudioFileData audio;
    audio.sample_rate = 48000;
    audio.channels.resize(1);
    audio.channels[0].assign(4096, 0.25f);
    if (!audio::write_wav_file(files.source.string(), audio,
                               audio::WavBitDepth::Float32)) {
        throw std::runtime_error("failed to create sampler API fixture");
    }
    return files;
}

}  // namespace

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

TEST_CASE("Sampler streaming runtime propagates detailed file admission results",
          "[audio][sampler][api][stream]") {
    auto files = make_sampler_api_wav();
    {
        std::ofstream invalid_sidecar(files.source.string() + ".pulpmip");
        REQUIRE(invalid_sidecar.good());
        invalid_sidecar << "not a valid sampler mip manifest";
    }

    examples::SamplerStreamingRuntime runtime;
    REQUIRE(runtime.load_sample_file_result("").status ==
            examples::PulpSamplerLoadStatus::EmptyPath);
    REQUIRE(runtime.load_sample_file_result(files.source.string()).status ==
            examples::PulpSamplerLoadStatus::NotPrepared);
    REQUIRE(runtime.prepare(48000.0f, 64));

    const auto loaded = runtime.load_sample_file_result(files.source.string());
    REQUIRE(loaded.loaded());
    REQUIRE(loaded.codec_capability ==
            examples::PulpSamplerCodecCapability::Ranged);
    REQUIRE_FALSE(loaded.codec().empty());
    REQUIRE(loaded.channels == 1);
    REQUIRE(loaded.sample_rate == 48000);
    REQUIRE(loaded.total_frames == 4096);
    REQUIRE(loaded.required_preload_frames > 0);
    REQUIRE(loaded.configured_preload_frames == loaded.total_frames);
    REQUIRE(loaded.requested_streaming_memory_bytes >=
            loaded.configured_preload_frames * sizeof(float));
    REQUIRE(loaded.sidecar_status ==
            examples::PulpSamplerSidecarStatus::IgnoredInvalid);
    REQUIRE(loaded.sidecar_level_count == 0);

    const auto published_before_failure = runtime.published_source();
    const auto unsupported = runtime.load_sample_file_result(
        files.source.string() + ".unsupported");
    REQUIRE(unsupported.status ==
            examples::PulpSamplerLoadStatus::UnsupportedCodec);
    const auto missing_wav = files.source.parent_path() /
        (files.source.stem().string() + "-missing.wav");
    const auto failed = runtime.load_sample_file_result(
        missing_wav.string());
    REQUIRE(failed.status == examples::PulpSamplerLoadStatus::OpenFailed);
    const auto published_after_failure = runtime.published_source();
    REQUIRE(published_after_failure.kind == published_before_failure.kind);
    REQUIRE(published_after_failure.selection_generation ==
            published_before_failure.selection_generation);
    REQUIRE(published_after_failure.streamed.asset.asset_id ==
            published_before_failure.streamed.asset.asset_id);
    REQUIRE(published_after_failure.streamed.asset.asset_generation ==
            published_before_failure.streamed.asset.asset_generation);
    REQUIRE_FALSE(runtime.load_sample_file(missing_wav.string()));
}
