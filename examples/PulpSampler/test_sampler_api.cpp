#include "sampler_api.hpp"
#include "sampler_heritage_state.hpp"
#include "sampler_streaming_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

audio::SampleHeritageProfile sampler_state_profile() {
    return {
        .schema_version = audio::kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.sampler-state-v1",
        .host_sample_rate = 48000.0,
        .stages = {
            {false, audio::SampleHeritageQuantizationStage{
                        12, 0.5f, 0x1234,
                        audio::SampleHeritageSeedPolicy::ContinueSerializedState}},
            {false, audio::SampleHeritageNoiseStage{
                        0.01f, 0x5678,
                        audio::SampleHeritageSeedPolicy::ContinueSerializedState}},
        },
    };
}

audio::SampleHeritageRuntimeState sampler_runtime_state(
    const audio::SampleHeritageProfile& profile) {
    const auto prepared = audio::validate_sample_heritage_profile(profile);
    if (!prepared.valid()) throw std::runtime_error("invalid heritage fixture");
    audio::SampleHeritageRuntimeState state;
    state.profile_schema_version = prepared.profile.schema_version;
    std::copy(prepared.profile.id().begin(), prepared.profile.id().end(),
              state.profile_id.begin());
    state.profile_digest = prepared.profile.profile_digest;
    state.rng_state_count = 2;
    state.rng_states[0] = {
        0, audio::SampleHeritageRuntimeRngStageType::Quantization, 0xabcdef};
    state.rng_states[1] = {
        1, audio::SampleHeritageRuntimeRngStageType::Noise, 0xfedcba};
    return state;
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void write_f64(std::vector<std::uint8_t>& bytes, std::size_t offset,
               double value) {
    auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(bits); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(bits & 0xffu);
        bits >>= 8;
    }
}

std::vector<std::uint8_t> sampler_state_envelope(std::string_view profile_json,
                                                  std::string_view runtime_json) {
    std::vector<std::uint8_t> bytes(examples::kSamplerHeritageStateHeaderBytes);
    write_u32(bytes, 0, examples::kSamplerHeritageStateMagic);
    bytes[4] = static_cast<std::uint8_t>(examples::kSamplerHeritageStateVersion);
    bytes[6] = runtime_json.empty()
        ? 0
        : static_cast<std::uint8_t>(examples::kSamplerHeritageStateHasRuntime);
    write_u32(bytes, 8, static_cast<std::uint32_t>(profile_json.size()));
    write_u32(bytes, 12, static_cast<std::uint32_t>(runtime_json.size()));
    write_f64(bytes, examples::kSamplerHeritageStateV1HeaderBytes,
              runtime_json.empty() ? 0.0 : 48000.0);
    bytes.insert(bytes.end(), profile_json.begin(), profile_json.end());
    bytes.insert(bytes.end(), runtime_json.begin(), runtime_json.end());
    return bytes;
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

TEST_CASE("PulpSampler heritage state round-trips canonical profile and RNG state",
          "[audio][sampler][api][heritage-state]") {
    examples::SamplerHeritagePersistentState source;
    source.enabled = true;
    source.profile = sampler_state_profile();
    source.has_runtime_state = true;
    source.runtime_state = sampler_runtime_state(source.profile);
    source.runtime_host_sample_rate = 48000.0;

    const auto written = examples::write_sampler_heritage_state(source);
    REQUIRE(written.valid());
    REQUIRE(written.bytes.size() > examples::kSamplerHeritageStateHeaderBytes);
    REQUIRE(written.bytes[0] == 'P');
    REQUIRE(written.bytes[1] == 'S');
    REQUIRE(written.bytes[2] == 'H');
    REQUIRE(written.bytes[3] == 'S');

    const auto parsed = examples::parse_sampler_heritage_state(written.bytes);
    REQUIRE(parsed.valid());
    REQUIRE(parsed.state.enabled);
    REQUIRE(parsed.state.has_runtime_state);
    REQUIRE(parsed.state.profile.profile_id == source.profile.profile_id);
    REQUIRE(parsed.state.runtime_state.profile_digest ==
            source.runtime_state.profile_digest);
    REQUIRE(parsed.state.runtime_state.rng_state_count == 2);
    REQUIRE(parsed.state.runtime_state.rng_states[1].random_state == 0xfedcba);
    REQUIRE(examples::write_sampler_heritage_state(parsed.state).bytes ==
            written.bytes);

    auto legacy_v1 = written.bytes;
    legacy_v1.erase(
        legacy_v1.begin() + examples::kSamplerHeritageStateV1HeaderBytes,
        legacy_v1.begin() + examples::kSamplerHeritageStateHeaderBytes);
    legacy_v1[4] = 1;
    const auto parsed_v1 = examples::parse_sampler_heritage_state(legacy_v1);
    REQUIRE(parsed_v1.valid());
    REQUIRE(parsed_v1.state.has_runtime_state);
    REQUIRE(parsed_v1.state.runtime_host_sample_rate == 0.0);
}

TEST_CASE("PulpSampler empty legacy heritage state restores disabled defaults",
          "[audio][sampler][api][heritage-state]") {
    const auto parsed = examples::parse_sampler_heritage_state({});
    REQUIRE(parsed.valid());
    REQUIRE_FALSE(parsed.state.enabled);
    REQUIRE_FALSE(parsed.state.has_runtime_state);

    examples::SamplerHeritagePersistentState disabled;
    const auto written = examples::write_sampler_heritage_state(disabled);
    REQUIRE(written.valid());
    REQUIRE(written.bytes.empty());
}

TEST_CASE("PulpSampler heritage envelope rejects malformed boundaries atomically",
          "[audio][sampler][api][heritage-state]") {
    examples::SamplerHeritagePersistentState source;
    source.enabled = true;
    source.profile = sampler_state_profile();
    const auto valid = examples::write_sampler_heritage_state(source).bytes;
    REQUIRE_FALSE(valid.empty());

    auto require_rejected = [](const std::vector<std::uint8_t>& bytes,
                               examples::SamplerHeritageStateStatus status) {
        const auto parsed = examples::parse_sampler_heritage_state(bytes);
        REQUIRE(parsed.status == status);
        REQUIRE_FALSE(parsed.state.enabled);
        REQUIRE_FALSE(parsed.state.has_runtime_state);
        REQUIRE(parsed.state.profile.profile_id.empty());
    };

    auto bad_magic = valid;
    bad_magic[0] ^= 0xff;
    require_rejected(bad_magic, examples::SamplerHeritageStateStatus::BadMagic);

    auto bad_version = valid;
    bad_version[4] = 3;
    require_rejected(bad_version,
                     examples::SamplerHeritageStateStatus::UnsupportedVersion);

    auto bad_flags = valid;
    bad_flags[6] = 0x80;
    require_rejected(bad_flags,
                     examples::SamplerHeritageStateStatus::UnsupportedFlags);

    require_rejected(std::vector<std::uint8_t>(valid.begin(), valid.begin() + 15),
                     examples::SamplerHeritageStateStatus::Truncated);
    auto truncated = valid;
    truncated.pop_back();
    require_rejected(truncated, examples::SamplerHeritageStateStatus::Truncated);

    auto overflow = valid;
    write_u32(overflow, 8, std::numeric_limits<std::uint32_t>::max());
    require_rejected(overflow,
                     examples::SamplerHeritageStateStatus::LengthOutOfRange);

    auto trailing = valid;
    trailing.push_back(0);
    require_rejected(trailing, examples::SamplerHeritageStateStatus::TrailingBytes);

    auto flag_length_disagree = valid;
    flag_length_disagree[6] = examples::kSamplerHeritageStateHasRuntime;
    require_rejected(flag_length_disagree,
                     examples::SamplerHeritageStateStatus::LengthOutOfRange);

    auto rate_without_runtime = valid;
    write_f64(rate_without_runtime,
              examples::kSamplerHeritageStateV1HeaderBytes, 44100.0);
    require_rejected(rate_without_runtime,
                     examples::SamplerHeritageStateStatus::LengthOutOfRange);
}

TEST_CASE("PulpSampler heritage state requires valid canonical JSON",
          "[audio][sampler][api][heritage-state]") {
    examples::SamplerHeritagePersistentState source;
    source.enabled = true;
    source.profile = sampler_state_profile();
    auto valid = examples::write_sampler_heritage_state(source).bytes;
    REQUIRE_FALSE(valid.empty());

    auto invalid = valid;
    invalid[examples::kSamplerHeritageStateHeaderBytes] = '!';
    REQUIRE(examples::parse_sampler_heritage_state(invalid).status ==
            examples::SamplerHeritageStateStatus::InvalidProfileJson);

    auto noncanonical = valid;
    noncanonical.insert(noncanonical.begin() +
                            examples::kSamplerHeritageStateHeaderBytes,
                        ' ');
    write_u32(noncanonical, 8,
              static_cast<std::uint32_t>(noncanonical.size() -
                                         examples::kSamplerHeritageStateHeaderBytes));
    REQUIRE(examples::parse_sampler_heritage_state(noncanonical).status ==
            examples::SamplerHeritageStateStatus::NonCanonicalProfileJson);
}

TEST_CASE("PulpSampler heritage state rejects runtime identity and layout mismatch",
          "[audio][sampler][api][heritage-state]") {
    examples::SamplerHeritagePersistentState source;
    source.enabled = true;
    source.profile = sampler_state_profile();
    source.has_runtime_state = true;
    source.runtime_state = sampler_runtime_state(source.profile);
    source.runtime_host_sample_rate = 48000.0;

    auto missing_runtime_rate = source;
    missing_runtime_rate.runtime_host_sample_rate = 0.0;
    REQUIRE(examples::write_sampler_heritage_state(missing_runtime_rate).status ==
            examples::SamplerHeritageStateStatus::LengthOutOfRange);

    auto wrong_digest = source;
    wrong_digest.runtime_state.profile_digest[0] ^= 0xff;
    REQUIRE(examples::write_sampler_heritage_state(wrong_digest).status ==
            examples::SamplerHeritageStateStatus::RuntimeProfileMismatch);
    REQUIRE(examples::write_sampler_heritage_state(wrong_digest).bytes.empty());

    auto wrong_layout = source;
    wrong_layout.runtime_state.rng_states[1].stage_index = 0;
    REQUIRE(examples::write_sampler_heritage_state(wrong_layout).status ==
            examples::SamplerHeritageStateStatus::RuntimeStageLayoutMismatch);
    REQUIRE(examples::write_sampler_heritage_state(wrong_layout).bytes.empty());

    auto invalid_runtime = source;
    invalid_runtime.runtime_state.rng_states[0].random_state = 0;
    REQUIRE(examples::write_sampler_heritage_state(invalid_runtime).status ==
            examples::SamplerHeritageStateStatus::InvalidRuntimeJson);
    REQUIRE(examples::write_sampler_heritage_state(invalid_runtime).bytes.empty());

    const auto profile_json =
        audio::write_sample_heritage_profile_json(source.profile);
    REQUIRE(profile_json.valid());

    const auto valid_runtime_json =
        audio::write_sample_heritage_runtime_state_json(source.runtime_state);
    REQUIRE(valid_runtime_json.valid());
    auto invalid_runtime_envelope = sampler_state_envelope(
        profile_json.json, valid_runtime_json.json);
    const auto profile_size = profile_json.json.size();
    invalid_runtime_envelope[examples::kSamplerHeritageStateHeaderBytes +
                             profile_size] = '!';
    REQUIRE(examples::parse_sampler_heritage_state(invalid_runtime_envelope).status ==
            examples::SamplerHeritageStateStatus::InvalidRuntimeJson);

    auto mismatched_runtime = source.runtime_state;
    mismatched_runtime.profile_digest[0] ^= 0xff;
    const auto mismatch_json =
        audio::write_sample_heritage_runtime_state_json(mismatched_runtime);
    REQUIRE(mismatch_json.valid());
    REQUIRE(examples::parse_sampler_heritage_state(
                sampler_state_envelope(profile_json.json, mismatch_json.json)).status ==
            examples::SamplerHeritageStateStatus::RuntimeProfileMismatch);

    auto mismatched_layout = source.runtime_state;
    mismatched_layout.rng_states[0].stage_type =
        audio::SampleHeritageRuntimeRngStageType::Noise;
    const auto layout_json =
        audio::write_sample_heritage_runtime_state_json(mismatched_layout);
    REQUIRE(layout_json.valid());
    REQUIRE(examples::parse_sampler_heritage_state(
                sampler_state_envelope(profile_json.json, layout_json.json)).status ==
            examples::SamplerHeritageStateStatus::RuntimeStageLayoutMismatch);
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
