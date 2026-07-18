#include <pulp/audio/sample_heritage.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

using namespace pulp::audio;

SampleHeritageProfile all_stage_profile(bool bypass) {
    return {
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.synthetic-v1",
        .host_sample_rate = 48000.0,
        .stages = {
            {bypass, SampleHeritageMachineDomainStage{48000.0}},
            {bypass, SampleHeritageQuantizationStage{12, 0.5f, 0x12345678u}},
            {bypass, SampleHeritageClockPitchStage{1.0}},
            {bypass, SampleHeritageDacHoldStage{2}},
            {bypass, SampleHeritageReconstructionFilterStage{12000.0}},
            {bypass, SampleHeritageNoiseStage{0.01f, 0xabcdefu}},
            {bypass, SampleHeritageOutputStage{0.75f}},
        },
    };
}

std::array<float, 16> render_noise(std::uint64_t seed) {
    SampleHeritageProfile profile{
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.seed-proof",
        .host_sample_rate = 48000.0,
        .stages = {{false, SampleHeritageNoiseStage{
            .amplitude = 0.1f,
            .seed = seed,
            .seed_policy = SampleHeritageSeedPolicy::RestartFromProfileSeed,
        }}},
    };
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    Buffer<float> output(1, 16);
    REQUIRE(engine.process(output.view()));
    std::array<float, 16> rendered{};
    std::copy(output.channel(0).begin(), output.channel(0).end(), rendered.begin());
    return rendered;
}

}  // namespace

TEST_CASE("Heritage profile validation produces one typed fixed prepared profile",
          "[audio][sampler][heritage]") {
    const auto validated = validate_sample_heritage_profile(all_stage_profile(true));
    REQUIRE(validated.valid());
    REQUIRE(validated.profile.schema_version == kSampleHeritageProfileSchemaVersion);
    REQUIRE(validated.profile.id() == "neutral.synthetic-v1");
    REQUIRE(validated.profile.stage_count == kSampleHeritageMaximumStages);

    auto invalid = all_stage_profile(true);
    invalid.profile_id = "Brand Model 1";
    REQUIRE(validate_sample_heritage_profile(invalid).status ==
            SampleHeritageProfileStatus::InvalidProfileId);
    invalid = all_stage_profile(true);
    invalid.schema_version += 1;
    REQUIRE(validate_sample_heritage_profile(invalid).status ==
            SampleHeritageProfileStatus::UnsupportedSchemaVersion);
    invalid = all_stage_profile(true);
    invalid.profile_id = "neutral..synthetic";
    REQUIRE(validate_sample_heritage_profile(invalid).status ==
            SampleHeritageProfileStatus::InvalidProfileId);
    invalid = all_stage_profile(true);
    invalid.stages.push_back({true, SampleHeritageOutputStage{}});
    REQUIRE(validate_sample_heritage_profile(invalid).status ==
            SampleHeritageProfileStatus::TooManyStages);
}

TEST_CASE("All heritage stages bypass bit-transparently",
          "[audio][sampler][heritage][bypass]") {
    const auto validated = validate_sample_heritage_profile(all_stage_profile(true));
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    Buffer<float> audio(2, 257);
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < audio.num_samples(); ++frame)
            audio.channel(channel)[frame] =
                static_cast<float>((frame * 17 + channel * 3) % 101) / 50.0f - 1.0f;
    const Buffer<float> reference = audio;
    REQUIRE(engine.process(audio.view()));
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < audio.num_samples(); ++frame)
            REQUIRE(audio.channel(channel)[frame] == reference.channel(channel)[frame]);
}

TEST_CASE("Heritage noise is deterministic for explicit seeds and reset policy",
          "[audio][sampler][heritage][seed]") {
    const auto first = render_noise(0x1234u);
    const auto second = render_noise(0x1234u);
    const auto other = render_noise(0x5678u);
    REQUIRE(first == second);
    REQUIRE(first != other);
    REQUIRE(std::any_of(first.begin(), first.end(), [](float value) { return value != 0.0f; }));

    auto profile = all_stage_profile(true);
    profile.stages = {{false, SampleHeritageNoiseStage{0.1f, 0x1234u}}};
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    Buffer<float> before(1, 8);
    REQUIRE(engine.process(before.view()));
    engine.reset();
    Buffer<float> after(1, 8);
    REQUIRE(engine.process(after.view()));
    for (std::size_t frame = 0; frame < 8; ++frame)
        REQUIRE(before.channel(0)[frame] == after.channel(0)[frame]);
}

TEST_CASE("Heritage validation fails unsupported conversion seams explicitly",
          "[audio][sampler][heritage][validation]") {
    auto profile = all_stage_profile(true);
    profile.stages[0].bypass = false;
    std::get<SampleHeritageMachineDomainStage>(profile.stages[0].parameters).sample_rate =
        32000.0;
    auto result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::UnsupportedRateConversion);
    REQUIRE(result.stage_index == 0);

    profile = all_stage_profile(true);
    profile.stages[2].bypass = false;
    std::get<SampleHeritageClockPitchStage>(profile.stages[2].parameters).ratio = 0.99;
    result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::UnsupportedRateConversion);
    REQUIRE(result.stage_index == 2);
}

TEST_CASE("Heritage duplicate stages and invalid deterministic seeds fail closed",
          "[audio][sampler][heritage][validation]") {
    auto profile = all_stage_profile(true);
    profile.stages[1] = {false, SampleHeritageNoiseStage{0.1f, 0x1234u}};
    auto result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::DuplicateStage);
    REQUIRE(result.stage_index == 5);

    profile = all_stage_profile(true);
    profile.stages[5] = {false, SampleHeritageNoiseStage{0.1f, 0}};
    result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::InvalidStageParameter);
    REQUIRE(result.stage_index == 5);

    profile = all_stage_profile(true);
    profile.stages[1] = {true, SampleHeritageQuantizationStage{
        12, 0.0f, 0, SampleHeritageSeedPolicy::ContinueSerializedState}};
    result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::InvalidStageParameter);
    REQUIRE(result.stage_index == 1);

    profile = all_stage_profile(true);
    profile.stages[5] = {true, SampleHeritageNoiseStage{
        0.0f, 0, SampleHeritageSeedPolicy::ContinueSerializedState}};
    result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::InvalidStageParameter);
    REQUIRE(result.stage_index == 5);
}

TEST_CASE("Heritage serialized seed state resumes the exact deterministic stream",
          "[audio][sampler][heritage][seed]") {
    SampleHeritageProfile profile{
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.serialized-seed",
        .host_sample_rate = 48000.0,
        .stages = {{false, SampleHeritageNoiseStage{
            .amplitude = 0.1f,
            .seed = 0x123456u,
            .seed_policy = SampleHeritageSeedPolicy::ContinueSerializedState,
        }}},
    };
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());

    SampleHeritageEngine uninterrupted;
    REQUIRE(uninterrupted.prepare(validated.profile));
    Buffer<float> prefix(1, 11);
    REQUIRE(uninterrupted.process(prefix.view()));
    const auto serialized = uninterrupted.capture_runtime_state();
    REQUIRE(serialized.valid());
    REQUIRE(serialized.state.rng_state_count == 1);
    REQUIRE(serialized.state.rng_states[0].random_state != 0);
    Buffer<float> expected(1, 13);
    REQUIRE(uninterrupted.process(expected.view()));

    SampleHeritageEngine restored;
    REQUIRE(restored.prepare(validated.profile));
    REQUIRE(restored.restore_runtime_state(serialized.state) ==
            SampleHeritageRuntimeStateStatus::Ok);
    Buffer<float> actual(1, 13);
    REQUIRE(restored.process(actual.view()));
    for (std::size_t frame = 0; frame < actual.num_samples(); ++frame)
        REQUIRE(actual.channel(0)[frame] == expected.channel(0)[frame]);
}

TEST_CASE("Prepared heritage stages allocate nothing in the callback",
          "[audio][sampler][heritage][rt]") {
    const auto validated = validate_sample_heritage_profile(all_stage_profile(false));
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    Buffer<float> audio(2, 512);
    for (std::size_t frame = 0; frame < audio.num_samples(); ++frame) {
        audio.channel(0)[frame] = std::sin(static_cast<float>(frame) * 0.1f) * 0.5f;
        audio.channel(1)[frame] = std::cos(static_cast<float>(frame) * 0.1f) * 0.5f;
    }
    bool processed = true;
    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback)
        processed = engine.process(audio.view()) && processed;
    REQUIRE(processed);
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("Heritage engine rejects forged prepared profiles before processing",
          "[audio][sampler][heritage][validation]") {
    auto validated = validate_sample_heritage_profile(all_stage_profile(true));
    REQUIRE(validated.valid());
    auto forged = validated.profile;
    forged.profile_id.fill('x');
    SampleHeritageEngine engine;
    REQUIRE_FALSE(engine.prepare(forged));

    forged = validated.profile;
    forged.stages[1].bypass = false;
    std::get<SampleHeritageQuantizationStage>(forged.stages[1].parameters).bit_depth = 0;
    REQUIRE_FALSE(engine.prepare(forged));
    REQUIRE_FALSE(engine.prepared());
}

TEST_CASE("Seeded heritage processing is invariant to callback partitioning",
          "[audio][sampler][heritage][seed]") {
    SampleHeritageProfile profile{
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.partition-proof",
        .host_sample_rate = 48000.0,
        .stages = {{false, SampleHeritageQuantizationStage{
            .bit_depth = 10,
            .dither_lsb = 0.5f,
            .seed = 0x998877u,
        }}},
    };
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine whole_engine;
    SampleHeritageEngine split_engine;
    REQUIRE(whole_engine.prepare(validated.profile));
    REQUIRE(split_engine.prepare(validated.profile));
    Buffer<float> whole(2, 32);
    for (std::size_t frame = 0; frame < whole.num_samples(); ++frame) {
        whole.channel(0)[frame] = static_cast<float>(frame) / 64.0f;
        whole.channel(1)[frame] = -static_cast<float>(frame) / 64.0f;
    }
    Buffer<float> split = whole;
    REQUIRE(whole_engine.process(whole.view()));
    REQUIRE(split_engine.process(split.view().slice(0, 11)));
    REQUIRE(split_engine.process(split.view().slice(11, 21)));
    for (std::size_t channel = 0; channel < whole.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < whole.num_samples(); ++frame)
            REQUIRE(whole.channel(channel)[frame] == split.channel(channel)[frame]);
}

TEST_CASE("Every synthetic heritage stage runs independently",
          "[audio][sampler][heritage][stages]") {
    const std::array<float, 4> input{0.12345f, 0.4f, -0.2f, 0.8f};
    const auto render = [&](SampleHeritageStageSpec stage) {
        SampleHeritageProfile profile{
            .schema_version = kSampleHeritageProfileSchemaVersion,
            .profile_id = "neutral.independent-stage",
            .host_sample_rate = 48000.0,
            .stages = {std::move(stage)},
        };
        const auto validated = validate_sample_heritage_profile(profile);
        REQUIRE(validated.valid());
        SampleHeritageEngine engine;
        REQUIRE(engine.prepare(validated.profile));
        Buffer<float> audio(1, input.size());
        std::copy(input.begin(), input.end(), audio.channel(0).begin());
        REQUIRE(engine.process(audio.view()));
        std::array<float, 4> output{};
        std::copy(audio.channel(0).begin(), audio.channel(0).end(), output.begin());
        return output;
    };

    REQUIRE(render({false, SampleHeritageMachineDomainStage{48000.0}}) == input);
    REQUIRE(render({false, SampleHeritageClockPitchStage{1.0}}) == input);
    REQUIRE(render({false, SampleHeritageQuantizationStage{8, 0.0f, 0}}) != input);
    REQUIRE(render({false, SampleHeritageDacHoldStage{2}}) != input);
    REQUIRE(render({false, SampleHeritageReconstructionFilterStage{12000.0}}) != input);
    REQUIRE(render({false, SampleHeritageNoiseStage{0.01f, 0x1234u}}) != input);
    REQUIRE(render({false, SampleHeritageOutputStage{0.5f}}) != input);
}
