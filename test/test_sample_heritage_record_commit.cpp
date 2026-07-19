#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/sample_heritage_record_commit.hpp>
#include <catch2/catch_approx.hpp>

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
    input.parameters = SampleHeritageRecordInputDriveClipBlock{.drive = 1.25f, .clip_level = 0.8f};
    profile.record_commit.push_back(input);

    SampleHeritageRecordCommitBlockSpec rate;
    rate.bypass = false;
    rate.parameters =
        SampleHeritageRecordRateBlock{.filter_family = SampleHeritageRecordFilterFamily::OnePole,
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
    const auto committed =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    REQUIRE(committed.valid());
    REQUIRE(committed.asset->audio().num_channels() == 2);
    REQUIRE(committed.asset->audio().num_samples() == 512);
    REQUIRE(committed.asset->metadata().committed_sample_rate == 24000.0);

    const auto loaded =
        reload_sample_heritage_committed_asset(profile, read_view(committed.asset->audio()), 24000.0,
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
    const auto first =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    const auto second =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
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
    rate.parameters =
        SampleHeritageRecordRateBlock{.filter_family = SampleHeritageRecordFilterFamily::Elliptic,
        .sample_rate = 48000.0,
        .cutoff_law = SampleHeritageCutoffLaw::FixedHz,
        .cutoff_value = 6000.0,
        .order = 8,
        .ripple_db = 1.0f,
        .stopband_attenuation_db = 60.0f};
    profile.record_commit.push_back(rate);

    const auto committed =
        commit_sample_heritage_recording(profile, read_view(impulse), 48000.0, provenance());
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
    const auto committed =
        commit_sample_heritage_recording(profile, read_view(source), 24000.0, provenance());
    REQUIRE(committed.valid());
    REQUIRE(committed.asset->audio().num_samples() == 1024);
    REQUIRE(committed.asset->metadata().committed_sample_rate == 48000.0);

    auto& invalid_rate =
        std::get<SampleHeritageRecordRateBlock>(profile.record_commit.front().parameters);
    invalid_rate.cutoff_law = SampleHeritageCutoffLaw::FixedHz;
    invalid_rate.cutoff_value = 16000.0;
    REQUIRE_FALSE(validate_sample_heritage_profile(profile).valid());
}

TEST_CASE("record commit reload rejects noncanonical metadata and altered audio",
          "[sample][heritage][record-commit]") {
    const auto profile = profile_with_record_chain();
    const auto source = deterministic_source();
    const auto committed =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
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
    std::get<SampleHeritageRecordConverterBlock>(changed_profile.record_commit[2].parameters)
        .seed += 1;
    const auto profile_mismatch =
        reload_sample_heritage_committed_asset(changed_profile, read_view(committed.asset->audio()), 24000.0,
        committed.asset->canonical_metadata_json());
    REQUIRE(profile_mismatch.status ==
            SampleHeritageRecordCommitStatus::ProfileMismatch);
    REQUIRE_FALSE(profile_mismatch.asset.has_value());
}

TEST_CASE("cyclic record stretch has exact lengths and a bit-exact identity",
          "[sample][heritage][record-commit][stretch]") {
    Buffer<float> source(1, 101);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame)
        source.channel(0)[frame] = static_cast<float>(frame) / 101.0f;

    for (const
    auto factor : {0.25, 0.75, 1.0, 1.25, 2.0, 20.0}) {
        SampleHeritageProfile profile;
        profile.profile_id = "neutral.cyclic-length";
        profile.host_sample_rate = 48000.0;
    SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = SampleHeritageRecordCommitCyclicStretchBlock{
        .factor = factor,
        .cycle_samples = factor == 1.0 ? 1000u : 7u,
            .crossfade_samples = factor == 1.0 ? 400u : 2u};
        profile.record_commit.push_back(stretch);
        const auto result =
            commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
        REQUIRE(result.valid());
        REQUIRE(result.asset->audio().num_samples() ==
                static_cast<std::size_t>(std::llround(101.0 * factor)));
        if (factor == 1.0)
            require_exact_audio(source, result.asset->audio());
    }
}

TEST_CASE("cyclic record stretch preserves zones and uses unit-speed grains",
          "[sample][heritage][record-commit][stretch]") {
    Buffer<float> source(1, 101);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame)
        source.channel(0)[frame] = static_cast<float>(frame);
    SampleHeritageProfile profile;
    profile.profile_id = "neutral.cyclic-zone";
    profile.host_sample_rate = 48000.0;
    SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = SampleHeritageRecordCommitCyclicStretchBlock{.factor = 2.0,
                                                                      .cycle_samples = 8,
                                                                      .crossfade_samples = 2,
        .zone_start_frame = 10,
        .zone_end_frame = 91};
    profile.record_commit.push_back(stretch);
    const auto result =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    REQUIRE(result.valid());
    REQUIRE(result.asset->audio().num_samples() == 182);
    REQUIRE(std::equal(source.channel(0).begin(), source.channel(0).begin() + 10,
                       result.asset->audio().channel(0).begin()));
    REQUIRE(result.asset->audio().channel(0)[172] == source.channel(0)[91]);
    REQUIRE(result.asset->audio().channel(0)[10] == source.channel(0)[10]);
    REQUIRE(result.asset->audio().channel(0)[11] == source.channel(0)[11]);

    const auto fade_in = std::pow(std::sin(3.14159265358979323846 / 8.0), 2.0);
    const auto expected_splice =
        source.channel(0)[16] * (1.0 - fade_in) + source.channel(0)[13] * fade_in;
    REQUIRE(result.asset->audio().channel(0)[16] == Catch::Approx(expected_splice).margin(1.0e-5));
}

TEST_CASE("adaptive record stretch has stable ties and a silent nominal offset",
          "[sample][heritage][record-commit][stretch][adaptive]") {
    const auto render = [](Buffer<float> source, std::uint32_t crossfade) {
        SampleHeritageProfile profile;
        profile.profile_id = "neutral.adaptive-decision";
        profile.host_sample_rate = 48000.0;
        SampleHeritageRecordCommitBlockSpec stretch;
        stretch.bypass = false;
        stretch.parameters =
            SampleHeritageRecordCommitAdaptiveStretchBlock{.factor = 2.0,
                                                           .decision_hop_samples = 8,
                                                           .search_radius_samples = 4,
                                                           .search_stride_samples = 1,
                                                           .crossfade_samples = crossfade};
        profile.record_commit.push_back(stretch);
        return commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    };

    Buffer<float> tied(1, 64);
    tied.channel(0)[0] = tied.channel(0)[8] = 0.2f;
    tied.channel(0)[1] = tied.channel(0)[9] = -0.4f;
    tied.channel(0)[2] = tied.channel(0)[10] = 0.7f;
    tied.channel(0)[3] = tied.channel(0)[11] = -0.1f;
    tied.channel(0)[4] = 0.25f;
    tied.channel(0)[12] = 0.75f;
    const auto tied_result = render(tied, 4);
    REQUIRE(tied_result.valid());
    REQUIRE(tied_result.asset->audio().channel(0)[12] == Catch::Approx(0.25f));

    Buffer<float> silent(1, 64);
    silent.channel(0)[10] = 1.0f;
    const auto silent_result = render(silent, 2);
    REQUIRE(silent_result.valid());
    REQUIRE(silent_result.asset->audio().channel(0)[14] == Catch::Approx(1.0f));
}

TEST_CASE("cyclic grains preserve pitch and have predictable impulse gains",
          "[sample][heritage][record-commit][stretch]") {
    const auto render = [](const Buffer<float>& source) {
        SampleHeritageProfile profile;
        profile.profile_id = "neutral.cyclic-grain";
        profile.host_sample_rate = 48000.0;
        SampleHeritageRecordCommitBlockSpec stretch;
        stretch.bypass = false;
        stretch.parameters = SampleHeritageRecordCommitCyclicStretchBlock{
            .factor = 2.0, .cycle_samples = 8, .crossfade_samples = 2};
        profile.record_commit.push_back(stretch);
        return commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    };

    Buffer<float> sine(1, 32);
    for (std::size_t frame = 0; frame < sine.num_samples(); ++frame)
        sine.channel(0)[frame] =
            static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frame / 7.0));
    const auto sine_result = render(sine);
    REQUIRE(sine_result.valid());
    REQUIRE(std::equal(sine.channel(0).begin(), sine.channel(0).begin() + 6,
                       sine_result.asset->audio().channel(0).begin()));

    Buffer<float> impulse(1, 32);
    impulse.channel(0)[3] = 1.0f;
    const auto impulse_result = render(impulse);
    REQUIRE(impulse_result.valid());
    REQUIRE(impulse_result.asset->audio().channel(0)[3] == 1.0f);
    const auto first_crossfade_gain = std::pow(std::sin(3.14159265358979323846 / 8.0), 2.0);
    REQUIRE(impulse_result.asset->audio().channel(0)[6] ==
            Catch::Approx(first_crossfade_gain).margin(1.0e-6));
}

TEST_CASE("adaptive record stretch is deterministic and preserves linked stereo",
          "[sample][heritage][record-commit][stretch][adaptive]") {
    Buffer<float> source(2, 127);
    for (std::size_t frame = 0; frame < source.num_samples(); ++frame) {
        source.channel(0)[frame] =
            static_cast<float>(0.4 * std::sin(2.0 * 3.14159265358979323846 * frame / 17.0));
        source.channel(1)[frame] = source.channel(0)[frame] * 0.5f;
    }
    SampleHeritageProfile profile;
    profile.profile_id = "neutral.adaptive-stereo";
    profile.host_sample_rate = 48000.0;
    SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = SampleHeritageRecordCommitAdaptiveStretchBlock{.factor = 1.25,
                                                                        .decision_hop_samples = 11,
                                                                        .search_radius_samples = 5,
                                                                        .search_stride_samples = 1,
                                                                        .crossfade_samples = 4,
        .stereo_link = true};
    profile.record_commit.push_back(stretch);
    const auto first =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    const auto second =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    require_exact_audio(first.asset->audio(), second.asset->audio());
    for (std::size_t frame = 0; frame < first.asset->audio().num_samples(); ++frame)
        REQUIRE(first.asset->audio().channel(1)[frame] ==
                Catch::Approx(first.asset->audio().channel(0)[frame] * 0.5f).margin(1.0e-6));

    std::get<SampleHeritageRecordCommitAdaptiveStretchBlock>(
        profile.record_commit.front().parameters)
        .stereo_link = false;
    source.channel(1)[31] += 0.3f;
    const auto unlinked_first =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    const auto unlinked_second =
        commit_sample_heritage_recording(profile, read_view(source), 48000.0, provenance());
    REQUIRE(unlinked_first.valid());
    REQUIRE(unlinked_second.valid());
    require_exact_audio(unlinked_first.asset->audio(), unlinked_second.asset->audio());
}

TEST_CASE("linked adaptive matching removes each channel DC offset independently",
          "[sample][heritage][record-commit][stretch][adaptive]") {
    Buffer<float> source(2, 64);
    constexpr std::array<float, 4> left_shape{-1.0f, 1.0f, -1.0f, 1.0f};
    constexpr std::array<float, 4> right_shape{1.0f, -1.0f, 1.0f, -1.0f};
    constexpr std::array<float, 4> misleading{0.2f, -0.1f, 0.3f, -0.2f};
    for (std::size_t frame = 0; frame < left_shape.size(); ++frame) {
        source.channel(0)[16 + frame] = left_shape[frame];
        source.channel(1)[16 + frame] = 100.0f + right_shape[frame];
        source.channel(0)[2 + frame] = 1000.0f + left_shape[frame];
        source.channel(1)[2 + frame] = -1000.0f + right_shape[frame];
        source.channel(0)[8 + frame] = misleading[frame];
        source.channel(1)[8 + frame] = 100.0f + misleading[frame];
    }
    source.channel(0)[6] = source.channel(1)[6] = 0.25f;
    source.channel(0)[12] = source.channel(1)[12] = 0.75f;

    SampleHeritageProfile profile;
    profile.profile_id = "neutral.adaptive-per-channel-dc";
    profile.host_sample_rate = 48000.0;
    SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = SampleHeritageRecordCommitAdaptiveStretchBlock{
        .factor = 2.0,
        .decision_hop_samples = 16,
        .search_radius_samples = 6,
        .search_stride_samples = 1,
        .crossfade_samples = 4,
        .stereo_link = true};
    profile.record_commit.push_back(stretch);

    const auto result = commit_sample_heritage_recording(
        profile, read_view(source), 48000.0, provenance());
    REQUIRE(result.valid());
    REQUIRE(result.asset->audio().channel(0)[20] == Catch::Approx(0.25f));
    REQUIRE(result.asset->audio().channel(1)[20] == Catch::Approx(0.25f));
}

TEST_CASE("record commit failures publish no partial asset",
          "[sample][heritage][record-commit]") {
    const auto source = deterministic_source();

    auto invalid_profile = profile_with_record_chain();
    invalid_profile.profile_id = "brand-model";
    const auto bad_profile =
        commit_sample_heritage_recording(invalid_profile, read_view(source), 48000.0, provenance());
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
    std::get<SampleHeritageRecordConverterBlock>(continuing.record_commit[2].parameters)
        .seed_policy =
        SampleHeritageSeedPolicy::ContinueSerializedState;
    const auto missing_state =
        commit_sample_heritage_recording(continuing, read_view(source), 48000.0, provenance());
    REQUIRE(missing_state.status ==
            SampleHeritageRecordCommitStatus::InvalidRecordChain);
    REQUIRE_FALSE(missing_state.asset.has_value());

    SampleHeritageProfile undersized;
    undersized.profile_id = "neutral.undersized-stretch";
    undersized.host_sample_rate = 48000.0;
    SampleHeritageRecordCommitBlockSpec stretch;
    stretch.bypass = false;
    stretch.parameters = SampleHeritageRecordCommitCyclicStretchBlock{
        .factor = 2.0, .cycle_samples = 2048, .crossfade_samples = 128};
    undersized.record_commit.push_back(stretch);
    const auto short_zone =
        commit_sample_heritage_recording(undersized, read_view(source), 48000.0, provenance());
    REQUIRE(short_zone.status == SampleHeritageRecordCommitStatus::InvalidRecordChain);
    REQUIRE_FALSE(short_zone.asset.has_value());

    auto outside = undersized;
    std::get<SampleHeritageRecordCommitCyclicStretchBlock>(
        outside.record_commit.front().parameters) = {.factor = 2.0,
                                                     .cycle_samples = 8,
                                                     .crossfade_samples = 2,
                                                     .zone_start_frame = 1000,
                                                     .zone_end_frame = 2000};
    const auto invalid_zone =
        commit_sample_heritage_recording(outside, read_view(source), 48000.0, provenance());
    REQUIRE(invalid_zone.status == SampleHeritageRecordCommitStatus::InvalidRecordChain);
    REQUIRE_FALSE(invalid_zone.asset.has_value());
}
