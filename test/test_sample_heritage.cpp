#include <pulp/audio/sample_heritage.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
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
