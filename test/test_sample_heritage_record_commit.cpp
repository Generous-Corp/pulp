#include <pulp/audio/sample_heritage_record_commit.hpp>
#include <pulp/audio/analysis/audio_spectrum.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

using namespace pulp::audio;

namespace {

BufferView<const float> read_view(const Buffer<float>& buffer) {
    return buffer.view();
}

SampleHeritageRecordProvenance provenance() {
    return {.source_id = "fixture:deterministic-stereo-tone",
            .capture_method = "synthetic-offline",
            .evidence_id = "evidence:record-load-null"};
}

SampleHeritageProfile profile_with_record_chain() {
    SampleHeritageProfile profile;
    profile.profile_id = "neutral.record-commit-test";
    profile.host_sample_rate = 48000.0;

    SampleHeritageRecordCommitBlockSpec input;
    input.bypass = false;
    input.parameters = SampleHeritageRecordInputDriveClipBlock{
        .drive = 1.25f, .clip_level = 0.8f};
    profile.record_commit.push_back(input);

    SampleHeritageRecordCommitBlockSpec rate;
    rate.bypass = false;
    rate.parameters = SampleHeritageRecordRateBlock{
        .filter_family = SampleHeritageRecordFilterFamily::OnePole,
        .sample_rate = 24000.0,
        .cutoff_law = SampleHeritageCutoffLaw::FixedHz,
        .cutoff_value = 9000.0,
        .order = 1,
        .ripple_db = 0.0f,
        .stopband_attenuation_db = 0.0f};
    profile.record_commit.push_back(rate);

    SampleHeritageRecordCommitBlockSpec converter;
    converter.bypass = false;
    converter.parameters = SampleHeritageRecordConverterBlock{
        .family = SampleHeritageConverterFamily::MuLaw,
        .bit_depth = 8.0f,
        .dac_nonlinearity = 0.15f,
        .dither_lsb = 0.5f,
        .seed = 0x5a17u,
        .seed_policy = SampleHeritageSeedPolicy::RestartFromProfileSeed};
    profile.record_commit.push_back(converter);
    return profile;
}

Buffer<float> deterministic_source() {
    constexpr std::size_t frames = 1024;
    Buffer<float> source(2, frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto phase = 2.0 * 3.14159265358979323846 *
                           static_cast<double>(frame) / 47.0;
        source.channel(0)[frame] = static_cast<float>(0.72 * std::sin(phase));
        source.channel(1)[frame] = static_cast<float>(0.51 * std::cos(phase * 0.73));
    }
    return source;
}

void require_exact_audio(const Buffer<float>& first, const Buffer<float>& second) {
    REQUIRE(first.num_channels() == second.num_channels());
    REQUIRE(first.num_samples() == second.num_samples());
    for (std::size_t channel = 0; channel < first.num_channels(); ++channel)
        REQUIRE(std::equal(first.channel(channel).begin(), first.channel(channel).end(),
                           second.channel(channel).begin()));
}

}  // namespace

TEST_CASE("record commit reloads to an exact synthetic null",
          "[sample][heritage][record-commit]") {
    const auto profile = profile_with_record_chain();
    const auto source = deterministic_source();
    const auto committed = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    REQUIRE(committed.valid());
    REQUIRE(committed.asset->audio().num_channels() == 2);
    REQUIRE(committed.asset->audio().num_samples() == 512);
    REQUIRE(committed.asset->metadata().committed_sample_rate == 24000.0);

    const auto loaded = reload_sample_heritage_committed_asset(
        profile, read_view(committed.asset->audio()), 24000.0,
        committed.asset->canonical_metadata_json());
    REQUIRE(loaded.valid());
    require_exact_audio(committed.asset->audio(), loaded.asset->audio());
    REQUIRE(loaded.asset->canonical_metadata_json() ==
            committed.asset->canonical_metadata_json());
}

TEST_CASE("record commit metadata and seeded conversion are deterministic",
          "[sample][heritage][record-commit][determinism]") {
    const auto profile = profile_with_record_chain();
    const auto source = deterministic_source();
    const auto first = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    const auto second = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    require_exact_audio(first.asset->audio(), second.asset->audio());
    REQUIRE(first.asset->canonical_metadata_json() ==
            second.asset->canonical_metadata_json());
    REQUIRE(first.asset->metadata().source_audio_sha256.size() == 64);
    REQUIRE(first.asset->metadata().committed_audio_sha256.size() == 64);
    REQUIRE(first.asset->metadata().profile_digest_sha256.size() == 64);
}

TEST_CASE("record commit elliptic attenuation is explicit and measurable",
          "[sample][heritage][record-commit][response]") {
    constexpr std::size_t frames = 16384;
    Buffer<float> impulse(1, frames);
    impulse.channel(0)[0] = 1.0f;

    SampleHeritageProfile profile;
    profile.profile_id = "neutral.record-filter-response";
    profile.host_sample_rate = 48000.0;
    SampleHeritageRecordCommitBlockSpec rate;
    rate.bypass = false;
    rate.parameters = SampleHeritageRecordRateBlock{
        .filter_family = SampleHeritageRecordFilterFamily::Elliptic,
        .sample_rate = 48000.0,
        .cutoff_law = SampleHeritageCutoffLaw::FixedHz,
        .cutoff_value = 6000.0,
        .order = 8,
        .ripple_db = 1.0f,
        .stopband_attenuation_db = 60.0f};
    profile.record_commit.push_back(rate);

    const auto committed = commit_sample_heritage_recording(
        profile, read_view(impulse), 48000.0, provenance());
    REQUIRE(committed.valid());
    constexpr std::array checkpoints{1000.0, 12000.0, 20000.0};
    pulp::test::audio::ResponseOptions options;
    options.fft_length = static_cast<int>(frames);
    const auto response = pulp::test::audio::response_relative_to_input(
        std::as_const(impulse).view(), read_view(committed.asset->audio()),
        48000.0, checkpoints, options);
    REQUIRE(response.magnitude_db_at(1000.0) > -1.5);
    REQUIRE(response.magnitude_db_at(12000.0) <= -55.0);
    REQUIRE(response.magnitude_db_at(20000.0) <= -55.0);
}

TEST_CASE("record commit validates and renders an upsample prefilter at the source rate",
          "[sample][heritage][record-commit][rate]") {
    Buffer<float> source(1, 512);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame)
        source.channel(0)[frame] = static_cast<float>(
            0.5 * std::sin(2.0 * 3.14159265358979323846 *
                           static_cast<double>(frame) / 19.0));

    SampleHeritageProfile profile;
    profile.profile_id = "neutral.record-upsample";
    profile.host_sample_rate = 24000.0;
    SampleHeritageRecordCommitBlockSpec rate;
    rate.bypass = false;
    rate.parameters = SampleHeritageRecordRateBlock{
        .filter_family = SampleHeritageRecordFilterFamily::Butterworth,
        .sample_rate = 48000.0,
        .cutoff_law = SampleHeritageCutoffLaw::MachineRateRatio,
        .cutoff_value = 0.4,
        .order = 4,
        .ripple_db = 0.0f,
        .stopband_attenuation_db = 0.0f};
    profile.record_commit.push_back(rate);

    REQUIRE(validate_sample_heritage_profile(profile).valid());
    const auto committed = commit_sample_heritage_recording(
        profile, read_view(source), 24000.0, provenance());
    REQUIRE(committed.valid());
    REQUIRE(committed.asset->audio().num_samples() == 1024);
    REQUIRE(committed.asset->metadata().committed_sample_rate == 48000.0);

    auto& invalid_rate = std::get<SampleHeritageRecordRateBlock>(
        profile.record_commit.front().parameters);
    invalid_rate.cutoff_law = SampleHeritageCutoffLaw::FixedHz;
    invalid_rate.cutoff_value = 16000.0;
    REQUIRE_FALSE(validate_sample_heritage_profile(profile).valid());
}

TEST_CASE("record commit reload rejects noncanonical metadata and altered audio",
          "[sample][heritage][record-commit]") {
    const auto profile = profile_with_record_chain();
    const auto source = deterministic_source();
    const auto committed = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    REQUIRE(committed.valid());

    const std::string spaced = std::string(committed.asset->canonical_metadata_json()) + " ";
    const auto noncanonical = reload_sample_heritage_committed_asset(
        profile, read_view(committed.asset->audio()), 24000.0, spaced);
    REQUIRE(noncanonical.status ==
            SampleHeritageRecordCommitStatus::NonCanonicalMetadata);
    REQUIRE_FALSE(noncanonical.asset.has_value());

    auto altered = committed.asset->audio();
    altered.channel(0)[23] += 0.01f;
    const auto mismatch = reload_sample_heritage_committed_asset(
        profile, read_view(altered), 24000.0,
        committed.asset->canonical_metadata_json());
    REQUIRE(mismatch.status == SampleHeritageRecordCommitStatus::AudioHashMismatch);
    REQUIRE_FALSE(mismatch.asset.has_value());

    auto changed_profile = profile;
    std::get<SampleHeritageRecordConverterBlock>(
        changed_profile.record_commit[2].parameters).seed += 1;
    const auto profile_mismatch = reload_sample_heritage_committed_asset(
        changed_profile, read_view(committed.asset->audio()), 24000.0,
        committed.asset->canonical_metadata_json());
    REQUIRE(profile_mismatch.status ==
            SampleHeritageRecordCommitStatus::ProfileMismatch);
    REQUIRE_FALSE(profile_mismatch.asset.has_value());
}

TEST_CASE("record commit rejects unsupported stretch instead of ignoring it",
          "[sample][heritage][record-commit]") {
    auto profile = profile_with_record_chain();
    SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = SampleHeritageRecordCommitStretchBlock{
        .family = SampleHeritageCommitStretchFamily::Cyclic,
        .factor = 1.0,
        .cycle_samples = 64,
        .splice_samples = 8,
        .quality = 0,
        .width = 0,
        .zone_start_frame = 0,
        .zone_end_frame = 0,
        .stereo_link = true};
    profile.record_commit.push_back(stretch);
    const auto source = deterministic_source();
    const auto result = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    REQUIRE(result.status ==
            SampleHeritageRecordCommitStatus::UnsupportedCommitStretch);
    REQUIRE_FALSE(result.asset.has_value());

    std::get<SampleHeritageRecordCommitStretchBlock>(
        profile.record_commit.back().parameters).factor = 2.0;
    const auto changed_factor = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    REQUIRE(changed_factor.status ==
            SampleHeritageRecordCommitStatus::UnsupportedCommitStretch);
}

TEST_CASE("record commit failures publish no partial asset",
          "[sample][heritage][record-commit]") {
    const auto source = deterministic_source();

    auto invalid_profile = profile_with_record_chain();
    invalid_profile.profile_id = "brand-model";
    const auto bad_profile = commit_sample_heritage_recording(
        invalid_profile, read_view(source), 48000.0, provenance());
    REQUIRE(bad_profile.status == SampleHeritageRecordCommitStatus::InvalidProfile);
    REQUIRE_FALSE(bad_profile.asset.has_value());

    const auto wrong_rate = commit_sample_heritage_recording(
        profile_with_record_chain(), read_view(source), 44100.0, provenance());
    REQUIRE(wrong_rate.status == SampleHeritageRecordCommitStatus::InvalidSource);
    REQUIRE_FALSE(wrong_rate.asset.has_value());

    auto nonfinite = source;
    nonfinite.channel(0)[0] = std::numeric_limits<float>::quiet_NaN();
    const auto bad_audio = commit_sample_heritage_recording(
        profile_with_record_chain(), read_view(nonfinite), 48000.0, provenance());
    REQUIRE(bad_audio.status == SampleHeritageRecordCommitStatus::InvalidSource);
    REQUIRE_FALSE(bad_audio.asset.has_value());

    auto continuing = profile_with_record_chain();
    std::get<SampleHeritageRecordConverterBlock>(
        continuing.record_commit[2].parameters).seed_policy =
        SampleHeritageSeedPolicy::ContinueSerializedState;
    const auto missing_state = commit_sample_heritage_recording(
        continuing, read_view(source), 48000.0, provenance());
    REQUIRE(missing_state.status ==
            SampleHeritageRecordCommitStatus::InvalidRecordChain);
    REQUIRE_FALSE(missing_state.asset.has_value());
}
