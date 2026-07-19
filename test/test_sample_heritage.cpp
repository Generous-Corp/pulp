#include <pulp/audio/sample_heritage.hpp>
#include <pulp/audio/analysis/audio_spectrum.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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

std::uint64_t oracle_next_random(std::uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

float oracle_bipolar_random(std::uint64_t& state) {
    const auto bits = static_cast<std::uint32_t>(oracle_next_random(state) >> 40);
    return static_cast<float>(bits) * (2.0f / 16777215.0f) - 1.0f;
}

SampleHeritageProfile rate_profile(double host_rate = 48000.0,
                                   double machine_rate = 32000.0,
                                   double clock_ratio = 1.25) {
    return {
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.rate-pipeline-v2",
        .host_sample_rate = host_rate,
        .stages = {
            {false, SampleHeritageMachineDomainStage{machine_rate}},
            {false, SampleHeritageClockPitchStage{clock_ratio}},
        },
    };
}

struct ExactRender {
    std::vector<float> output;
    std::size_t input_frames = 0;
    std::size_t machine_frames = 0;
};

struct ClockTransitionRender {
    std::vector<float> output;
    std::size_t latency = 0;
};

ExactRender render_exact(const SampleHeritagePreparedProfile& profile,
                         std::span<const std::size_t> partitions,
                         std::size_t maximum_output_frames) {
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({profile, 1, maximum_output_frames}) ==
            SampleHeritagePrepareStatus::Ok);
    std::vector<float> input(engine.maximum_input_frames());
    std::vector<float> output(maximum_output_frames);
    ExactRender rendered;
    std::uint64_t source_cursor = 0;
    for (const auto frames : partitions) {
        const auto plan = engine.plan_exact(frames);
        REQUIRE(plan.valid());
        REQUIRE(plan.input_frames <= input.size());
        for (std::size_t frame = 0; frame < plan.input_frames; ++frame) {
            const auto position = static_cast<double>(source_cursor + frame);
            input[frame] = static_cast<float>(
                0.45 * std::sin(position * 0.031) +
                0.15 * std::cos(position * 0.007));
        }
        const float* input_pointer = input.data();
        float* output_pointer = output.data();
        const BufferView<const float> input_view(&input_pointer, 1,
                                                 plan.input_frames);
        BufferView<float> output_view(&output_pointer, 1, frames);
        REQUIRE(engine.process_exact(plan, input_view, output_view) ==
                SampleHeritageProcessStatus::Ok);
        rendered.output.insert(rendered.output.end(), output.begin(),
                               output.begin() + static_cast<std::ptrdiff_t>(frames));
        rendered.input_frames += plan.input_frames;
        rendered.machine_frames += plan.machine_frames;
        source_cursor += plan.input_frames;
    }
    return rendered;
}

SampleHeritageProfile typed_voice_profile(
    std::vector<SampleHeritageVoiceBlockSpec> voice) {
    return {
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.typed-voice-dsp",
        .host_sample_rate = 48000.0,
        .voice = std::move(voice),
    };
}

std::vector<float> render_typed_voice(
    std::vector<SampleHeritageVoiceBlockSpec> voice,
    std::span<const float> input,
    std::span<const std::size_t> partitions = {}) {
    const auto validated = validate_sample_heritage_profile(
        typed_voice_profile(std::move(voice)));
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    Buffer<float> buffer(1, input.size());
    std::copy(input.begin(), input.end(), buffer.channel(0).begin());
    if (partitions.empty()) {
        REQUIRE(engine.process(buffer.view()));
    } else {
        std::size_t offset = 0;
        for (const auto frames : partitions) {
            REQUIRE(engine.process(buffer.view().slice(offset, frames)));
            offset += frames;
        }
        REQUIRE(offset == input.size());
    }
    return {buffer.channel(0).begin(), buffer.channel(0).end()};
}

ClockTransitionRender render_clock_transition(
    double initial_multiplier,
    double transitioned_multiplier,
    double maximum_factor,
    bool step,
    std::span<const std::size_t> transition_partitions) {
    const auto profile = typed_voice_profile({
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoicePitchBlock{
             SampleHeritagePitchFamily::VariableClock}},
    });
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({
                .profile = validated.profile,
                .channel_count = 1,
                .maximum_output_frames = 4096,
                .maximum_runtime_clock_factor = maximum_factor,
            }) == SampleHeritagePrepareStatus::Ok);

    const auto prelude_plan = engine.plan_exact(32, initial_multiplier);
    REQUIRE(prelude_plan.valid());
    Buffer<float> prelude_input(1, prelude_plan.input_frames);
    Buffer<float> prelude_output(1, prelude_plan.output_frames);
    REQUIRE(engine.process_exact(prelude_plan,
                                 std::as_const(prelude_input).view(),
                                 prelude_output.view()) ==
            SampleHeritageProcessStatus::Ok);

    ClockTransitionRender rendered;
    rendered.latency = static_cast<std::size_t>(engine.latency_output_frames());
    bool first_input = true;
    for (const auto frames : transition_partitions) {
        const auto plan = engine.plan_exact(frames, transitioned_multiplier);
        REQUIRE(plan.valid());
        Buffer<float> input(1, plan.input_frames);
        Buffer<float> output(1, plan.output_frames);
        if (step) {
            std::fill(input.channel(0).begin(), input.channel(0).end(), 1.0f);
        } else if (first_input && input.num_samples() != 0) {
            input.channel(0)[0] = 1.0f;
        }
        first_input = first_input && input.num_samples() == 0;
        REQUIRE(engine.process_exact(plan, std::as_const(input).view(),
                                     output.view()) ==
                SampleHeritageProcessStatus::Ok);
        rendered.output.insert(rendered.output.end(), output.channel(0).begin(),
                               output.channel(0).end());
    }
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

TEST_CASE("Heritage validation accepts bounded machine and clock conversions",
          "[audio][sampler][heritage][validation]") {
    auto profile = all_stage_profile(true);
    profile.stages[0].bypass = false;
    std::get<SampleHeritageMachineDomainStage>(profile.stages[0].parameters).sample_rate =
        32000.0;
    auto result = validate_sample_heritage_profile(profile);
    REQUIRE(result.valid());

    profile = all_stage_profile(true);
    profile.stages[2].bypass = false;
    std::get<SampleHeritageClockPitchStage>(profile.stages[2].parameters).ratio = 0.99;
    result = validate_sample_heritage_profile(profile);
    REQUIRE(result.valid());

    profile = all_stage_profile(true);
    profile.stages[0].bypass = false;
    std::get<SampleHeritageMachineDomainStage>(profile.stages[0].parameters).sample_rate =
        8000.0;
    profile.stages[2].bypass = false;
    std::get<SampleHeritageClockPitchStage>(profile.stages[2].parameters).ratio = 1000.0;
    result = validate_sample_heritage_profile(profile);
    REQUIRE(result.status == SampleHeritageProfileStatus::UnsupportedRateConversion);
}

TEST_CASE("Heritage schema v2 enforces canonical topology and portable identity",
          "[audio][sampler][heritage][schema]") {
    auto reordered = rate_profile();
    std::swap(reordered.stages[0], reordered.stages[1]);
    const auto invalid = validate_sample_heritage_profile(reordered);
    REQUIRE(invalid.status == SampleHeritageProfileStatus::InvalidStageOrder);
    REQUIRE(invalid.stage_index == 1);

    auto first = rate_profile(44100.0, 32000.0, 1.0);
    auto second = first;
    second.host_sample_rate = 96000.0;
    const auto first_validated = validate_sample_heritage_profile(first);
    const auto second_validated = validate_sample_heritage_profile(second);
    REQUIRE(first_validated.valid());
    REQUIRE(second_validated.valid());
    REQUIRE(first_validated.profile.profile_digest ==
            second_validated.profile.profile_digest);

    auto clock_domain_filter = rate_profile(48000.0, 32000.0, 0.5);
    clock_domain_filter.stages.push_back(
        {false, SampleHeritageReconstructionFilterStage{9000.0}});
    const auto invalid_filter =
        validate_sample_heritage_profile(clock_domain_filter);
    REQUIRE(invalid_filter.status ==
            SampleHeritageProfileStatus::InvalidStageParameter);
    REQUIRE(invalid_filter.stage_index == 2);
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

    const std::array<float, 4> quantized{
        16.0f / 128.0f, 51.0f / 128.0f,
        -26.0f / 128.0f, 102.0f / 128.0f,
    };
    REQUIRE(render({false, SampleHeritageQuantizationStage{8, 0.0f, 0}}) ==
            quantized);

    std::uint64_t quantizer_state = 0x1234u;
    std::array<float, 4> dithered_quantized{};
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto dither = oracle_bipolar_random(quantizer_state) * 0.5f;
        dithered_quantized[index] =
            std::round(std::clamp(input[index], -1.0f, 127.0f / 128.0f) *
                           128.0f +
                       dither) /
            128.0f;
    }
    REQUIRE(render({false, SampleHeritageQuantizationStage{8, 0.5f, 0x1234u}}) ==
            dithered_quantized);

    const std::array<float, 4> held{input[0], input[0], input[2], input[2]};
    REQUIRE(render({false, SampleHeritageDacHoldStage{2}}) == held);

    const auto pole = static_cast<float>(
        std::exp(-2.0 * std::numbers::pi * 12000.0 / 48000.0));
    std::array<float, 4> filtered{};
    float previous = 0.0f;
    for (std::size_t index = 0; index < input.size(); ++index) {
        previous = (1.0f - pole) * input[index] + pole * previous;
        filtered[index] = previous;
    }
    REQUIRE(render({false, SampleHeritageReconstructionFilterStage{12000.0}}) ==
            filtered);

    std::uint64_t noise_state = 0x1234u;
    std::array<float, 4> noisy{};
    for (std::size_t index = 0; index < input.size(); ++index) {
        noisy[index] = input[index] +
                       oracle_bipolar_random(noise_state) * 0.01f;
    }
    REQUIRE(render({false, SampleHeritageNoiseStage{0.01f, 0x1234u}}) == noisy);

    std::array<float, 4> scaled{};
    for (std::size_t index = 0; index < input.size(); ++index)
        scaled[index] = input[index] * 0.5f;
    REQUIRE(render({false, SampleHeritageOutputStage{0.5f}}) == scaled);
}

TEST_CASE("Typed heritage converters quantize fractional and companded codes",
          "[audio][sampler][heritage][converter]") {
    constexpr std::array<float, 7> input{
        -1.0f, -0.031f, -0.004f, 0.0f, 0.004f, 0.031f, 0.9f};
    const auto converter = [](SampleHeritageConverterFamily family,
                              float bit_depth,
                              float nonlinearity = 0.0f,
                              float dither = 0.0f) {
        return SampleHeritageVoiceBlockSpec{
            SampleHeritageBlockDomain::Voice, false,
            SampleHeritageVoiceConverterBlock{
                family, bit_depth, nonlinearity, dither,
                dither == 0.0f ? 0u : 0x123456u}};
    };

    const auto fractional = render_typed_voice(
        {converter(SampleHeritageConverterFamily::LinearPcm, 7.5f)}, input);
    const auto levels = std::exp2(6.5f);
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto expected = std::clamp(
            std::round(std::clamp(input[index], -1.0f, 1.0f) * levels),
            -std::floor(levels), std::floor(levels - 1.0f)) / levels;
        REQUIRE(fractional[index] == expected);
    }

    const auto linear = render_typed_voice(
        {converter(SampleHeritageConverterFamily::LinearPcm, 4.0f)}, input);
    const auto mu_law = render_typed_voice(
        {converter(SampleHeritageConverterFamily::MuLaw, 4.0f)}, input);
    const auto a_law = render_typed_voice(
        {converter(SampleHeritageConverterFamily::ALaw, 4.0f)}, input);
    REQUIRE(linear != mu_law);
    REQUIRE(mu_law != a_law);
    REQUIRE(linear[4] == 0.0f);
    REQUIRE(mu_law[4] != 0.0f);
    REQUIRE(a_law[4] != 0.0f);

    constexpr std::array partitions{std::size_t{2}, std::size_t{1},
                                    std::size_t{4}};
    const auto dithered_whole = render_typed_voice(
        {converter(SampleHeritageConverterFamily::MuLaw, 6.25f, 0.0f, 0.75f)},
        input);
    const auto dithered_split = render_typed_voice(
        {converter(SampleHeritageConverterFamily::MuLaw, 6.25f, 0.0f, 0.75f)},
        input, partitions);
    REQUIRE(dithered_whole == dithered_split);

    const auto nonlinear = render_typed_voice(
        {converter(SampleHeritageConverterFamily::LinearPcm, 16.0f, 1.0f)},
        input);
    REQUIRE(std::all_of(nonlinear.begin(), nonlinear.end(),
                        [](float sample) {
                            return std::isfinite(sample) && sample >= -1.0f &&
                                   sample <= 1.0f;
                        }));
    REQUIRE(nonlinear[5] > linear[5]);
}

TEST_CASE("Typed heritage hold applies deterministic per-machine-sample droop",
          "[audio][sampler][heritage][hold]") {
    constexpr std::array<float, 8> input{1.0f, 0.8f, 0.6f, 0.4f,
                                         -1.0f, 0.0f, 0.0f, 0.0f};
    const auto block = SampleHeritageVoiceBlockSpec{
        SampleHeritageBlockDomain::Voice, false,
        SampleHeritageVoiceHoldDroopBlock{
            SampleHeritageHoldMode::ZeroOrder, 4, 0.25f}};
    const auto output = render_typed_voice({block}, input);
    const std::array<float, 8> expected{1.0f, 0.75f, 0.5625f, 0.421875f,
                                       -1.0f, -0.75f, -0.5625f, -0.421875f};
    REQUIRE(std::equal(output.begin(), output.end(), expected.begin()));
    constexpr std::array partitions{std::size_t{3}, std::size_t{2},
                                    std::size_t{3}};
    const auto split = render_typed_voice({block}, input, partitions);
    REQUIRE(std::equal(split.begin(), split.end(), expected.begin()));
}

TEST_CASE("Typed heritage reconstruction families have bounded stable responses",
          "[audio][sampler][heritage][filter]") {
    constexpr std::size_t frames = 16384;
    Buffer<float> impulse(1, frames);
    impulse.channel(0)[0] = 1.0f;
    constexpr std::array checkpoints{1000.0, 6000.0, 12000.0, 20000.0};
    const auto response = [&](SampleHeritageReconstructionFamily family,
                              SampleHeritageCutoffLaw law,
                              double cutoff,
                              std::uint8_t order,
                              float ripple,
                              float attenuation) {
        const auto profile = typed_voice_profile({{
            SampleHeritageBlockDomain::Voice, false,
            SampleHeritageVoiceReconstructionBlock{
                family, law, cutoff, order, ripple, attenuation}}});
        const auto validated = validate_sample_heritage_profile(profile);
        REQUIRE(validated.valid());
        SampleHeritageEngine engine;
        REQUIRE(engine.prepare(validated.profile));
        Buffer<float> output = impulse;
        REQUIRE(engine.process(output.view()));
        pulp::test::audio::ResponseOptions options;
        options.fft_length = static_cast<int>(frames);
        return pulp::test::audio::response_relative_to_input(
            std::as_const(impulse).view(), std::as_const(output).view(),
            48000.0, checkpoints, options);
    };

    const auto one_pole = response(
        SampleHeritageReconstructionFamily::OnePole,
        SampleHeritageCutoffLaw::FixedHz, 6000.0, 1, 0.0f, 0.0f);
    REQUIRE(one_pole.magnitude_db_at(1000.0) > -1.0);
    REQUIRE(one_pole.magnitude_db_at(20000.0) < -8.0);

    const auto butterworth = response(
        SampleHeritageReconstructionFamily::Butterworth,
        SampleHeritageCutoffLaw::MachineRateRatio, 0.125, 8, 0.0f, 0.0f);
    REQUIRE(butterworth.magnitude_db_at(1000.0) > -0.2);
    REQUIRE(butterworth.magnitude_db_at(12000.0) < -40.0);

    const auto chebyshev = response(
        SampleHeritageReconstructionFamily::Chebyshev,
        SampleHeritageCutoffLaw::FixedHz, 6000.0, 8, 1.0f, 0.0f);
    REQUIRE(chebyshev.magnitude_db_at(12000.0) < -45.0);
    REQUIRE(chebyshev.magnitude_db_at(6000.0) !=
            butterworth.magnitude_db_at(6000.0));

    const auto elliptic = response(
        SampleHeritageReconstructionFamily::Elliptic,
        SampleHeritageCutoffLaw::FixedHz, 6000.0, 8, 1.0f, 60.0f);
    REQUIRE(elliptic.magnitude_db_at(12000.0) <= -55.0);
    REQUIRE(elliptic.magnitude_db_at(20000.0) <= -55.0);
}

TEST_CASE("Typed heritage analog color is normalized bounded and measurable",
          "[audio][sampler][heritage][analog][thd]") {
    constexpr std::size_t frames = 16384;
    constexpr double sample_rate = 48000.0;
    constexpr double frequency = sample_rate * 127.0 / frames;
    std::vector<float> input(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        input[frame] = static_cast<float>(
            0.65 * std::sin(2.0 * std::numbers::pi * frequency * frame /
                            sample_rate));
    const auto dry = render_typed_voice(
        {{SampleHeritageBlockDomain::Voice, false,
          SampleHeritageVoiceAnalogColorBlock{4.0f, 0.45f, 0.0f}}}, input);
    REQUIRE(std::equal(dry.begin(), dry.end(), input.begin()));
    const auto colored = render_typed_voice(
        {{SampleHeritageBlockDomain::Voice, false,
          SampleHeritageVoiceAnalogColorBlock{4.0f, 0.45f, 1.0f}}}, input);
    REQUIRE(std::all_of(colored.begin(), colored.end(), [](float sample) {
        return std::isfinite(sample) && sample >= -1.0f && sample <= 1.0f;
    }));
    Buffer<float> dry_buffer(1, frames);
    Buffer<float> colored_buffer(1, frames);
    std::copy(dry.begin(), dry.end(), dry_buffer.channel(0).begin());
    std::copy(colored.begin(), colored.end(), colored_buffer.channel(0).begin());
    pulp::test::audio::ThdOptions options;
    options.fft_length = static_cast<int>(frames);
    const auto dry_thd = pulp::test::audio::measure_thd(
        std::as_const(dry_buffer).view(), frequency, sample_rate, options);
    const auto colored_thd = pulp::test::audio::measure_thd(
        std::as_const(colored_buffer).view(), frequency, sample_rate, options);
    REQUIRE(colored_thd.thd_db() > dry_thd.thd_db() + 30.0);
    REQUIRE(colored_thd.thd_percent() > 1.0);

    constexpr std::array<float, 3> edge_input{-0.5f, 0.0f, 0.5f};
    const auto finite_edge = render_typed_voice(
        {{SampleHeritageBlockDomain::Voice, false,
          SampleHeritageVoiceAnalogColorBlock{
              std::numeric_limits<float>::denorm_min(), 1.0f, 1.0f}}},
        edge_input);
    REQUIRE(std::equal(finite_edge.begin(), finite_edge.end(),
                       edge_input.begin()));
    REQUIRE(std::all_of(finite_edge.begin(), finite_edge.end(),
                        [](float sample) { return std::isfinite(sample); }));
}

TEST_CASE("Active typed live cyclic stretch fails preparation until implemented",
          "[audio][sampler][heritage][validation]") {
    const auto profile = typed_voice_profile({{
        SampleHeritageBlockDomain::Voice, false,
        SampleHeritageVoiceLiveCyclicStretchBlock{
            1.5, 12.0, 2.0, true, false, 0, 0}}});
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE_FALSE(engine.prepare(validated.profile));
}

TEST_CASE("Typed heritage tails use an audible-silence bound and hard time cap",
          "[audio][sampler][heritage][tail]") {
    const auto profile = typed_voice_profile({
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoiceHoldDroopBlock{
             SampleHeritageHoldMode::ZeroOrder, 65536, 0.0f}},
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoiceReconstructionBlock{
             SampleHeritageReconstructionFamily::OnePole,
             SampleHeritageCutoffLaw::FixedHz, 1.0, 1, 0.0f, 0.0f}},
    });
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({validated.profile, 1, 64}) ==
            SampleHeritagePrepareStatus::Ok);
    REQUIRE(engine.tail_output_frames() >= 65536);
    REQUIRE(engine.tail_output_frames() <= 480000);
}

TEST_CASE("Typed heritage tail processing suppresses converter excitation",
          "[audio][sampler][heritage][tail][seed]") {
    const auto profile = typed_voice_profile({
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoiceConverterBlock{
             SampleHeritageConverterFamily::LinearPcm, 8.0f, 0.0f, 1.0f,
             0x123456u,
             SampleHeritageSeedPolicy::ContinueSerializedState}},
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoiceReconstructionBlock{
             SampleHeritageReconstructionFamily::OnePole,
             SampleHeritageCutoffLaw::FixedHz, 2000.0, 1, 0.0f, 0.0f}},
    });
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({validated.profile, 1, 64}) ==
            SampleHeritagePrepareStatus::Ok);

    auto plan = engine.plan_exact(1);
    Buffer<float> impulse(1, plan.input_frames);
    Buffer<float> first_output(1, 1);
    impulse.channel(0)[0] = 1.0f;
    REQUIRE(engine.process_exact(plan, std::as_const(impulse).view(),
                                 first_output.view()) ==
            SampleHeritageProcessStatus::Ok);
    const auto rng_before_tail = engine.capture_runtime_state();
    REQUIRE(rng_before_tail.valid());

    std::uint64_t remaining = engine.tail_output_frames();
    float final_sample = 1.0f;
    while (remaining != 0) {
        const auto frames = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, 64));
        plan = engine.plan_exact(frames);
        Buffer<float> zero_input(1, plan.input_frames);
        Buffer<float> output(1, frames);
        REQUIRE(engine.process_tail_exact(plan, std::as_const(zero_input).view(),
                                          output.view()) ==
                SampleHeritageProcessStatus::Ok);
        final_sample = output.channel(0).back();
        remaining -= frames;
    }
    REQUIRE(std::abs(final_sample) <=
            SampleHeritageVoiceDsp::kTailSilenceThreshold);
    const auto rng_after_tail = engine.capture_runtime_state();
    REQUIRE(rng_after_tail.valid());
    REQUIRE(rng_after_tail.state.rng_states[0].random_state ==
            rng_before_tail.state.rng_states[0].random_state);

    plan = engine.plan_exact(1);
    Buffer<float> nonzero(1, plan.input_frames);
    Buffer<float> rejected_output(1, 1);
    nonzero.channel(0)[0] = 0.25f;
    REQUIRE(engine.process_tail_exact(plan, std::as_const(nonzero).view(),
                                      rejected_output.view()) ==
            SampleHeritageProcessStatus::TailInputNotSilent);
}

TEST_CASE("Heritage causal two-leg SRC is bitwise partition invariant",
          "[audio][sampler][heritage][src][partition]") {
    const auto validated = validate_sample_heritage_profile(rate_profile());
    REQUIRE(validated.valid());
    constexpr std::array whole{std::size_t{1024}};
    constexpr std::array split{
        std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{3},
        std::size_t{257}, std::size_t{128}, std::size_t{511}, std::size_t{53},
    };
    static_assert(1 + 7 + 64 + 3 + 257 + 128 + 511 + 53 == 1024);
    const auto one = render_exact(validated.profile, whole, 1024);
    const auto many = render_exact(validated.profile, split, 1024);
    REQUIRE(one.output == many.output);
    REQUIRE(one.input_frames == many.input_frames);
    REQUIRE(one.machine_frames == many.machine_frames);
    REQUIRE(one.input_frames > 1024);
    REQUIRE(std::any_of(one.output.begin(), one.output.end(),
                        [](float sample) { return sample != 0.0f; }));
}

TEST_CASE("Runtime heritage clock has invariant latency and exact identity epoch",
          "[audio][sampler][heritage][src][clock]") {
    const auto profile = typed_voice_profile({
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoicePitchBlock{
             SampleHeritagePitchFamily::VariableClock}},
    });
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({
                .profile = validated.profile,
                .channel_count = 1,
                .maximum_output_frames = 512,
                .maximum_runtime_clock_factor = 4.0,
            }) == SampleHeritagePrepareStatus::Ok);
    REQUIRE(engine.latency_output_frames() == 96.0);
    REQUIRE(engine.latency_output_frames(0.25) == 96.0);
    REQUIRE(engine.latency_output_frames(4.0) == 96.0);

    const auto plan = engine.plan_exact(384, 1.0);
    REQUIRE(plan.valid());
    REQUIRE(plan.input_frames == 384);
    Buffer<float> input(1, plan.input_frames);
    Buffer<float> output(1, plan.output_frames);
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
        input.channel(0)[frame] = static_cast<float>(frame + 1) / 512.0f;
    REQUIRE(engine.process_exact(plan, std::as_const(input).view(), output.view()) ==
            SampleHeritageProcessStatus::Ok);
    for (std::size_t frame = 0; frame < 96; ++frame)
        REQUIRE(output.channel(0)[frame] == 0.0f);
    for (std::size_t frame = 96; frame < output.num_samples(); ++frame)
        REQUIRE(output.channel(0)[frame] == input.channel(0)[frame - 96]);

    const auto changed = engine.plan_exact(64, 2.0);
    REQUIRE(changed.valid());
    Buffer<float> changed_input(1, changed.input_frames);
    Buffer<float> changed_output(1, changed.output_frames);
    REQUIRE(engine.process_exact(changed, std::as_const(changed_input).view(),
                                 changed_output.view()) ==
            SampleHeritageProcessStatus::Ok);

    auto forged = engine.plan_exact(32, 1.5);
    REQUIRE(forged.valid());
    forged.runtime_clock_multiplier = 1.25;
    Buffer<float> forged_input(1, forged.input_frames);
    Buffer<float> forged_output(1, forged.output_frames);
    REQUIRE(engine.process_exact(forged, std::as_const(forged_input).view(),
                                 forged_output.view()) ==
            SampleHeritageProcessStatus::InvalidPlan);
    REQUIRE(engine.plan_exact(32, 0.249).status != SampleHeritagePlanStatus::Ok);
    REQUIRE(engine.plan_exact(32, 4.001).status != SampleHeritagePlanStatus::Ok);
}

TEST_CASE("Runtime heritage clock is bitwise partition invariant at fixed factors",
          "[audio][sampler][heritage][src][clock][partition]") {
    const auto validated = validate_sample_heritage_profile(typed_voice_profile({
        {SampleHeritageBlockDomain::Voice, false,
         SampleHeritageVoicePitchBlock{
             SampleHeritagePitchFamily::VariableClock}},
    }));
    REQUIRE(validated.valid());
    const auto render = [&](std::span<const std::size_t> partitions,
                            double multiplier) {
        SampleHeritageEngine engine;
        REQUIRE(engine.prepare({
                    .profile = validated.profile,
                    .channel_count = 1,
                    .maximum_output_frames = 512,
                    .maximum_runtime_clock_factor = 4.0,
                }) == SampleHeritagePrepareStatus::Ok);
        std::vector<float> rendered;
        std::uint64_t source_cursor = 0;
        for (const auto frames : partitions) {
            const auto plan = engine.plan_exact(frames, multiplier);
            REQUIRE(plan.valid());
            Buffer<float> input(1, plan.input_frames);
            Buffer<float> output(1, plan.output_frames);
            for (std::size_t frame = 0; frame < input.num_samples(); ++frame)
                input.channel(0)[frame] = static_cast<float>(
                    0.6 * std::sin((source_cursor + frame) * 0.071));
            source_cursor += plan.input_frames;
            REQUIRE(engine.process_exact(plan, std::as_const(input).view(),
                                         output.view()) ==
                    SampleHeritageProcessStatus::Ok);
            rendered.insert(rendered.end(), output.channel(0).begin(),
                            output.channel(0).end());
        }
        return rendered;
    };
    constexpr std::array whole{std::size_t{512}};
    constexpr std::array split{std::size_t{7}, std::size_t{31},
                               std::size_t{128}, std::size_t{3},
                               std::size_t{343}};
    REQUIRE(render(whole, 1.0) == render(split, 1.0));
    REQUIRE(render(whole, 1.75) == render(split, 1.75));
    REQUIRE(render(whole, 0.375) == render(split, 0.375));
}

TEST_CASE("Runtime clock transitions preserve invariant measured latency",
          "[audio][sampler][heritage][src][clock][latency]") {
    const auto verify_impulse = [](double from, double to, double maximum) {
        const auto expected_latency = static_cast<std::size_t>(std::ceil(
            static_cast<double>(kHighQualitySampleSincHalfWidth) * maximum));
        const std::array partitions{expected_latency + 64};
        const auto rendered = render_clock_transition(
            from, to, maximum, false, partitions);
        REQUIRE(rendered.latency == expected_latency);
        const auto peak = static_cast<std::size_t>(std::distance(
            rendered.output.begin(),
            std::max_element(rendered.output.begin(), rendered.output.end(),
                             [](float left, float right) {
                                 return std::abs(left) < std::abs(right);
                             })));
        REQUIRE(peak == expected_latency);
    };

    verify_impulse(1.0, 64.0, 64.0);
    verify_impulse(64.0, 1.0, 64.0);
    verify_impulse(1.5, 0.75, 4.0);

    constexpr std::size_t expected_latency =
        kHighQualitySampleSincHalfWidth * 64;
    const std::array partitions{expected_latency + 64};
    const auto step = render_clock_transition(
        1.0, 64.0, 64.0, true, partitions);
    const auto crossing = static_cast<std::size_t>(std::distance(
        step.output.begin(),
        std::find_if(step.output.begin(), step.output.end(),
                     [](float sample) { return sample >= 0.5f; })));
    REQUIRE(crossing == expected_latency);
}

TEST_CASE("Runtime clock transition correction is partition invariant",
          "[audio][sampler][heritage][src][clock][partition]") {
    constexpr std::size_t latency = kHighQualitySampleSincHalfWidth * 4;
    const std::array whole{latency + 79};
    const std::array split{
        std::size_t{1}, std::size_t{7}, std::size_t{31},
        std::size_t{64}, latency + 79 - 1 - 7 - 31 - 64};
    const auto one = render_clock_transition(1.5, 0.75, 4.0, false, whole);
    const auto many = render_clock_transition(1.5, 0.75, 4.0, false, split);
    REQUIRE(one.latency == many.latency);
    REQUIRE(one.output == many.output);
}

TEST_CASE("Heritage exact plans are bounded single-use contracts",
          "[audio][sampler][heritage][src][contract]") {
    const auto validated = validate_sample_heritage_profile(rate_profile());
    REQUIRE(validated.valid());
    SampleHeritageEngine legacy;
    REQUIRE_FALSE(legacy.prepare(validated.profile));
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({validated.profile, 1, 128}) ==
            SampleHeritagePrepareStatus::Ok);
    REQUIRE(engine.machine_sample_rate() == 32000.0);
    REQUIRE(engine.clocked_sample_rate() == 40000.0);
    REQUIRE(engine.clock_ratio() == 1.25);
    REQUIRE(std::abs(engine.latency_output_frames() - 48.0) < 1.0e-12);
    REQUIRE(engine.tail_output_frames() >= 48);
    REQUIRE(engine.tail_output_frames() <
            std::numeric_limits<std::uint64_t>::max());

    const auto plan = engine.plan_exact(64);
    REQUIRE(plan.valid());
    REQUIRE(plan.input_frames > 0);
    REQUIRE(plan.input_frames <= engine.maximum_input_frames());
    REQUIRE(plan.machine_frames <= engine.maximum_machine_frames());
    std::vector<float> input(plan.input_frames, 0.25f);
    std::vector<float> output(64, -1.0f);
    const float* input_pointer = input.data();
    float* output_pointer = output.data();
    const BufferView<const float> short_input(&input_pointer, 1,
                                              plan.input_frames - 1);
    BufferView<float> output_view(&output_pointer, 1, output.size());
    REQUIRE(engine.process_exact(plan, short_input, output_view) ==
            SampleHeritageProcessStatus::InputFrameMismatch);
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float sample) { return sample == -1.0f; }));

    const BufferView<const float> input_view(&input_pointer, 1, input.size());
    REQUIRE(engine.process_exact(plan, input_view, output_view) ==
            SampleHeritageProcessStatus::Ok);
    REQUIRE(engine.process_exact(plan, input_view, output_view) ==
            SampleHeritageProcessStatus::InvalidPlan);

    const auto stale = engine.plan_exact(8);
    engine.reset();
    REQUIRE(engine.process_exact(stale, input_view, output_view) ==
            SampleHeritageProcessStatus::InvalidPlan);
    const auto reset_plan = engine.plan_exact(64);
    REQUIRE(reset_plan.input_frames == plan.input_frames);
    REQUIRE(reset_plan.machine_frames == plan.machine_frames);
    std::vector<float> reset_output(64, -1.0f);
    float* reset_output_pointer = reset_output.data();
    BufferView<float> reset_output_view(&reset_output_pointer, 1,
                                        reset_output.size());
    REQUIRE(engine.process_exact(reset_plan, input_view, reset_output_view) ==
            SampleHeritageProcessStatus::Ok);
    REQUIRE(reset_output == output);
    REQUIRE(engine.plan_exact(129).status ==
            SampleHeritagePlanStatus::OutputTooLarge);
}

TEST_CASE("Heritage engines can share one immutable sinc kernel bank",
          "[audio][sampler][heritage][src]") {
    const auto validated = validate_sample_heritage_profile(rate_profile());
    REQUIRE(validated.valid());
    SampleSincKernelBank shared_bank;
    REQUIRE(shared_bank.build_dense_for_maximum_consumption(1.5));
    const auto shared_view = shared_bank.view();

    SampleHeritageEngine first;
    SampleHeritageEngine second;
    const SampleHeritagePrepareConfig config{
        validated.profile, 1, 128, &shared_view};
    REQUIRE(first.prepare(config) == SampleHeritagePrepareStatus::Ok);
    REQUIRE(second.prepare(config) == SampleHeritagePrepareStatus::Ok);

    const auto render = [](SampleHeritageEngine& engine) {
        const auto plan = engine.plan_exact(128);
        REQUIRE(plan.valid());
        std::vector<float> input(plan.input_frames);
        std::vector<float> output(plan.output_frames);
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = static_cast<float>(
                0.4 * std::sin(static_cast<double>(frame) * 0.037));
        const float* input_pointer = input.data();
        float* output_pointer = output.data();
        const BufferView<const float> input_view(&input_pointer, 1, input.size());
        BufferView<float> output_view(&output_pointer, 1, output.size());
        REQUIRE(engine.process_exact(plan, input_view, output_view) ==
                SampleHeritageProcessStatus::Ok);
        return std::pair{plan, output};
    };

    const auto first_render = render(first);
    const auto second_render = render(second);
    REQUIRE(first_render.first.status == second_render.first.status);
    REQUIRE(first_render.first.output_frames ==
            second_render.first.output_frames);
    REQUIRE(first_render.first.input_frames ==
            second_render.first.input_frames);
    REQUIRE(first_render.first.machine_frames ==
            second_render.first.machine_frames);
    REQUIRE(first_render.second == second_render.second);

    first.reset();
    second.reset();
    const auto first_reset = render(first);
    const auto second_reset = render(second);
    REQUIRE(first_reset.first.input_frames ==
            first_render.first.input_frames);
    REQUIRE(first_reset.first.machine_frames ==
            first_render.first.machine_frames);
    REQUIRE(first_reset.second == first_render.second);
    REQUIRE(second_reset.second == second_render.second);

    first.release();
    second.reset();
    const auto second_after_peer_release = render(second);
    REQUIRE(second_after_peer_release.second == second_render.second);
    REQUIRE(shared_view.select(1.5).valid());
}

TEST_CASE("External heritage sinc banks fail closed when invalid or undersized",
          "[audio][sampler][heritage][src]") {
    const auto validated = validate_sample_heritage_profile(rate_profile());
    REQUIRE(validated.valid());

    const SampleSincKernelBankView invalid_view;
    SampleHeritageEngine invalid_engine;
    REQUIRE(invalid_engine.prepare(
                {validated.profile, 1, 64, &invalid_view}) ==
            SampleHeritagePrepareStatus::KernelBuildFailed);
    REQUIRE_FALSE(invalid_engine.prepared());

    SampleSincKernelBank undersized_bank;
    REQUIRE(undersized_bank.build_dense_for_maximum_consumption(1.0));
    const auto undersized_view = undersized_bank.view();
    SampleHeritageEngine undersized_engine;
    REQUIRE(undersized_engine.prepare(
                {validated.profile, 1, 64, &undersized_view}) ==
            SampleHeritagePrepareStatus::KernelBuildFailed);
    REQUIRE_FALSE(undersized_engine.prepared());
}

TEST_CASE("All-bypass exact heritage processing is direct and bit transparent",
          "[audio][sampler][heritage][src][bypass]") {
    auto profile = rate_profile();
    for (auto& stage : profile.stages) stage.bypass = true;
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({validated.profile, 2, 257}) ==
            SampleHeritagePrepareStatus::Ok);
    REQUIRE(engine.latency_output_frames() == 0.0);
    REQUIRE(engine.tail_output_frames() == 0);
    const auto plan = engine.plan_exact(257);
    REQUIRE(plan.valid());
    REQUIRE(plan.input_frames == 257);
    REQUIRE(plan.machine_frames == 0);
    Buffer<float> input(2, 257);
    Buffer<float> output(2, 257);
    for (std::size_t channel = 0; channel < 2; ++channel)
        for (std::size_t frame = 0; frame < 257; ++frame)
            input.channel(channel)[frame] =
                static_cast<float>(channel * 257 + frame) * 0.001f - 0.2f;
    const auto input_view = static_cast<const Buffer<float>&>(input).view();
    REQUIRE(engine.process_exact(plan, input_view, output.view()) ==
            SampleHeritageProcessStatus::Ok);
    REQUIRE(std::equal(input.channel(0).begin(), input.channel(0).end(),
                       output.channel(0).begin()));
    REQUIRE(std::equal(input.channel(1).begin(), input.channel(1).end(),
                       output.channel(1).begin()));
}

TEST_CASE("Prepared exact heritage SRC allocates nothing in process calls",
          "[audio][sampler][heritage][src][rt]") {
    const auto validated = validate_sample_heritage_profile(rate_profile());
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare({validated.profile, 1, 64}) ==
            SampleHeritagePrepareStatus::Ok);
    std::vector<float> input(engine.maximum_input_frames(), 0.1f);
    std::array<float, 64> output{};
    const float* input_pointer = input.data();
    float* output_pointer = output.data();
    bool processed = true;
    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        const auto plan = engine.plan_exact(output.size());
        const BufferView<const float> input_view(&input_pointer, 1,
                                                 plan.input_frames);
        BufferView<float> output_view(&output_pointer, 1, output.size());
        processed = plan.valid() &&
            engine.process_exact(plan, input_view, output_view) ==
                SampleHeritageProcessStatus::Ok && processed;
    }
    REQUIRE(processed);
    REQUIRE_FALSE(probe.saw_allocation());
}

namespace {

SampleHeritagePreparedProfile prepared_bus_profile(
    SampleHeritageBusNoiseIdleBlock noise,
    std::optional<SampleHeritageBusOutputDriveBlock> output = std::nullopt) {
    SampleHeritageProfile profile{
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.bus-dsp-v3",
        .host_sample_rate = 48000.0,
        .bus = {{SampleHeritageBlockDomain::Bus, false, noise}},
    };
    if (output.has_value())
        profile.bus.push_back(
            {SampleHeritageBlockDomain::Bus, false, *output});
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    return validated.profile;
}

std::vector<float> render_bus(SampleHeritageBusDsp& dsp,
                              std::size_t frames,
                              bool any_voice_active,
                              float input = 0.0f) {
    std::vector<float> output(frames, input);
    float* pointer = output.data();
    BufferView<float> view(&pointer, 1, output.size());
    REQUIRE(dsp.process(view, any_voice_active) ==
            SampleHeritageBusDspStatus::Ok);
    return output;
}

}  // namespace

TEST_CASE("Typed heritage bus noise and idle obey the voice-active gate",
          "[audio][sampler][heritage][bus][gate]") {
    const auto make = [](float noise, float idle,
                         SampleHeritageNoiseGate gate) {
        return prepared_bus_profile({
            .noise_amplitude = noise,
            .idle_amplitude = idle,
            .tilt_db_per_octave = 0.0f,
            .gate = gate,
            .seed = 0x12345678u,
            .seed_policy = SampleHeritageSeedPolicy::RestartFromProfileSeed,
            .tilt_reference_hz = 1000.0,
            .tilt_floor_hz = 20.0,
        });
    };

    SampleHeritageBusDsp gated_noise;
    REQUIRE(gated_noise.prepare(
                make(0.1f, 0.0f, SampleHeritageNoiseGate::VoiceActive),
                48000.0, 1) == SampleHeritageBusDspStatus::Ok);
    REQUIRE(render_bus(gated_noise, 128, false) ==
            std::vector<float>(128, 0.0f));
    REQUIRE(render_bus(gated_noise, 128, true) !=
            std::vector<float>(128, 0.0f));

    SampleHeritageBusDsp idle;
    REQUIRE(idle.prepare(
                make(0.0f, 0.1f, SampleHeritageNoiseGate::VoiceActive),
                48000.0, 1) == SampleHeritageBusDspStatus::Ok);
    REQUIRE(render_bus(idle, 128, false) != std::vector<float>(128, 0.0f));

    SampleHeritageBusDsp simultaneous;
    REQUIRE(simultaneous.prepare(
                make(0.1f, 0.1f, SampleHeritageNoiseGate::VoiceActive),
                48000.0, 1) == SampleHeritageBusDspStatus::Ok);
    const auto active = render_bus(simultaneous, 128, true);
    simultaneous.reset();
    const auto inactive = render_bus(simultaneous, 128, false);
    REQUIRE(active != inactive);

    SampleHeritageBusDsp always;
    REQUIRE(always.prepare(
                make(0.1f, 0.0f, SampleHeritageNoiseGate::AlwaysOn),
                48000.0, 1) == SampleHeritageBusDspStatus::Ok);
    REQUIRE(render_bus(always, 128, false) != std::vector<float>(128, 0.0f));
}

TEST_CASE("Typed heritage bus tilt follows its versioned shelf cascade",
          "[audio][sampler][heritage][bus][response]") {
    REQUIRE(kSampleHeritageSpectralTiltLawVersion == 1);
    const auto profile = prepared_bus_profile({
        .noise_amplitude = 0.1f,
        .idle_amplitude = 0.0f,
        .tilt_db_per_octave = -3.0f,
        .gate = SampleHeritageNoiseGate::AlwaysOn,
        .seed = 0x1234u,
        .seed_policy = SampleHeritageSeedPolicy::RestartFromProfileSeed,
        .tilt_reference_hz = 1000.0,
        .tilt_floor_hz = 20.0,
    });
    SampleHeritageBusDsp dsp;
    REQUIRE(dsp.prepare(profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    REQUIRE(std::abs(dsp.tilt_response_db(0, 1000.0)) < 1.0e-9);
    for (const auto frequency : {125.0, 250.0, 500.0, 2000.0, 4000.0,
                                 8000.0}) {
        const auto expected = -3.0 * std::log2(frequency / 1000.0);
        INFO("frequency: " << frequency);
        INFO("measured dB: " << dsp.tilt_response_db(0, frequency));
        INFO("expected dB: " << expected);
        REQUIRE(std::abs(dsp.tilt_response_db(0, frequency) - expected) < 1.5);
    }
    const auto below_floor = dsp.tilt_response_db(0, 1.0);
    const auto near_asymptote = dsp.tilt_response_db(0, 5.0);
    const auto at_floor = dsp.tilt_response_db(0, 20.0);
    REQUIRE(std::abs(below_floor - near_asymptote) < 0.01);
    REQUIRE(std::abs(below_floor - at_floor) > 0.5);
}

TEST_CASE("Typed heritage bus RNG is partition invariant and restorable",
          "[audio][sampler][heritage][bus][state][partition]") {
    const auto profile = prepared_bus_profile({
        .noise_amplitude = 0.03f,
        .idle_amplitude = 0.01f,
        .tilt_db_per_octave = -3.0f,
        .gate = SampleHeritageNoiseGate::VoiceActive,
        .seed = 0xabcdefu,
        .seed_policy = SampleHeritageSeedPolicy::ContinueSerializedState,
        .tilt_reference_hz = 1000.0,
        .tilt_floor_hz = 20.0,
    });
    SampleHeritageBusDsp whole;
    SampleHeritageBusDsp split;
    REQUIRE(whole.prepare(profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    REQUIRE(split.prepare(profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    const auto contiguous = render_bus(whole, 512, true);
    std::vector<float> partitioned;
    for (const auto frames : {std::size_t{17}, std::size_t{63},
                              std::size_t{128}, std::size_t{304}}) {
        const auto block = render_bus(split, frames, true);
        partitioned.insert(partitioned.end(), block.begin(), block.end());
    }
    REQUIRE(contiguous == partitioned);

    whole.reset();
    REQUIRE(render_bus(whole, 512, true) == contiguous);

    const auto state_profile = prepared_bus_profile({
        .noise_amplitude = 0.03f,
        .idle_amplitude = 0.01f,
        .tilt_db_per_octave = 0.0f,
        .gate = SampleHeritageNoiseGate::VoiceActive,
        .seed = 0xabcdefu,
        .seed_policy = SampleHeritageSeedPolicy::ContinueSerializedState,
        .tilt_reference_hz = 1000.0,
        .tilt_floor_hz = 20.0,
    });
    SampleHeritageBusDsp state_source;
    REQUIRE(state_source.prepare(state_profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    (void)render_bus(state_source, 512, true);
    SampleHeritageRuntimeEngineState state;
    REQUIRE(state_source.capture_runtime_state(state) ==
            SampleHeritageRuntimeStateStatus::Ok);
    const auto continuation = render_bus(state_source, 257, false);
    SampleHeritageBusDsp restored;
    REQUIRE(restored.prepare(state_profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    REQUIRE(restored.restore_runtime_state(state) ==
            SampleHeritageRuntimeStateStatus::Ok);
    REQUIRE(render_bus(restored, 257, false) == continuation);

}

TEST_CASE("Typed heritage bus output drive is exact below its ceiling and clips",
          "[audio][sampler][heritage][bus][drive][thd]") {
    const SampleHeritageBusNoiseIdleBlock bypassed_noise{};
    const auto unity_profile = prepared_bus_profile(
        bypassed_noise, SampleHeritageBusOutputDriveBlock{1.0f, 0.8f});
    SampleHeritageBusDsp unity;
    REQUIRE(unity.prepare(unity_profile, 48000.0, 1) ==
            SampleHeritageBusDspStatus::Ok);
    std::array<float, 7> values{-1.0f, -0.8f, -0.25f, 0.0f,
                                0.25f, 0.8f, 1.0f};
    float* values_pointer = values.data();
    BufferView<float> values_view(&values_pointer, 1, values.size());
    REQUIRE(unity.process(values_view, false) ==
            SampleHeritageBusDspStatus::Ok);
    constexpr std::array<float, 7> expected{
        -0.8f, -0.8f, -0.25f, 0.0f, 0.25f, 0.8f, 0.8f};
    REQUIRE(values == expected);

    constexpr std::size_t frames = 16384;
    constexpr double sample_rate = 48000.0;
    constexpr double frequency = sample_rate * 127.0 / frames;
    Buffer<float> dry(1, frames);
    Buffer<float> driven(1, frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto sample = static_cast<float>(
            0.65 * std::sin(2.0 * std::numbers::pi * frequency * frame /
                            sample_rate));
        dry.channel(0)[frame] = sample;
        driven.channel(0)[frame] = sample;
    }
    const auto driven_profile = prepared_bus_profile(
        bypassed_noise, SampleHeritageBusOutputDriveBlock{2.0f, 0.8f});
    SampleHeritageBusDsp drive;
    REQUIRE(drive.prepare(driven_profile, sample_rate, 1) ==
            SampleHeritageBusDspStatus::Ok);
    REQUIRE(drive.process(driven.view(), true) ==
            SampleHeritageBusDspStatus::Ok);
    REQUIRE(std::all_of(driven.channel(0).begin(), driven.channel(0).end(),
                        [](float sample) { return std::abs(sample) <= 0.8f; }));
    pulp::test::audio::ThdOptions options;
    options.fft_length = static_cast<int>(frames);
    const auto dry_thd = pulp::test::audio::measure_thd(
        std::as_const(dry).view(), frequency, sample_rate, options);
    const auto driven_thd = pulp::test::audio::measure_thd(
        std::as_const(driven).view(), frequency, sample_rate, options);
    REQUIRE(driven_thd.thd_percent() > 1.0);
    REQUIRE(driven_thd.thd_db() > dry_thd.thd_db() + 30.0);
}

TEST_CASE("Prepared typed heritage bus processing allocates nothing",
          "[audio][sampler][heritage][bus][rt]") {
    const auto profile = prepared_bus_profile(
        {.noise_amplitude = 0.03f,
         .idle_amplitude = 0.01f,
         .tilt_db_per_octave = 4.0f,
         .gate = SampleHeritageNoiseGate::VoiceActive,
         .seed = 0x1234u,
         .seed_policy = SampleHeritageSeedPolicy::ContinueSerializedState,
         .tilt_reference_hz = 1000.0,
         .tilt_floor_hz = 20.0},
        SampleHeritageBusOutputDriveBlock{2.0f, 0.9f});
    SampleHeritageBusDsp dsp;
    REQUIRE(dsp.prepare(profile, 48000.0, 2) ==
            SampleHeritageBusDspStatus::Ok);
    Buffer<float> buffer(2, 128);
    bool processed = true;
    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        buffer.clear();
        processed = dsp.process(buffer.view(), (callback & 1) != 0) ==
                        SampleHeritageBusDspStatus::Ok &&
                    processed;
    }
    REQUIRE(processed);
    REQUIRE_FALSE(probe.saw_allocation());
}
