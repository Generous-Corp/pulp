#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/dawn_shared_io_wavenet_spec.hpp"
#include "detail/shared_io_arena.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

using namespace pulp::gpu_audio::detail;

namespace {
struct Fixture {
    std::array<std::uint32_t, 2> dilations{1, 2};
    DawnSharedIoWavenetLayerSpec layer{
        .input_size = 1,
        .condition_size = 1,
        .channels = 2,
        .kernel = 2,
        .head_size = 1,
        .gated = 1,
        .head_bias = 1,
        .dilations = dilations,
    };
    // rechannel 2 + 2 layers * (4*2*2 + 4 + 4 + 4 + 2) + head (2 + 1) + scale
    std::vector<float> weights = std::vector<float>(static_cast<std::size_t>(2 + 2 * 30 + 4), 0.0f);

    Fixture() {
        weights.back() = 1.0f;
    }
};

struct TinyReference {
    float a0_l0 = 0.0f;
    std::array<float, 2> a0_l1{};
    std::size_t a0_l1_head = 0;
    float a1_l0 = 0.0f;

    float process(float input) {
        const float a0_input = input;
        const float z0 = 0.2f * a0_l0 + 0.4f * a0_input + 0.1f + 0.05f * input;
        a0_l0 = a0_input;
        const float h0 = std::tanh(z0);
        const float r0 = a0_input + 0.3f * h0 - 0.02f;

        const float z1 = -0.1f * a0_l1[a0_l1_head] + 0.25f * r0 - 0.03f + 0.04f * input;
        a0_l1[a0_l1_head] = r0;
        a0_l1_head = (a0_l1_head + 1u) % a0_l1.size();
        const float h1 = std::tanh(z1);
        const float r1 = r0 + 0.2f * h1 + 0.01f;
        const float array0_head = 0.7f * (h0 + h1);

        const float a1_input = 0.8f * r1;
        const float z2 = 0.15f * a1_l0 + 0.35f * a1_input + 0.02f - 0.05f * input;
        a1_l0 = a1_input;
        const float h2 = std::tanh(z2);
        const float array1_head = 0.6f * (array0_head + h2) - 0.04f;
        return 0.5f * array1_head;
    }
};
} // namespace

TEST_CASE("WaveNet shared spec accepts the authenticated flat-weight shape",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    CHECK(validate_dawn_shared_io_wavenet_spec(spec).accepted());
#if defined(PULP_GPU_AUDIO_WAVENET_RUNTIME)
    auto program = DawnSharedIoWavenetProgram::create(spec);
    REQUIRE(program);
    CHECK(program->weight_count() == fixture.weights.size());
    CHECK(program->history_bytes() != 0);
#endif
}

TEST_CASE("WaveNet shared spec rejects mismatched flat weights before allocation",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.weights.pop_back();
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    CHECK(validate_dawn_shared_io_wavenet_spec(spec).error ==
          DawnSharedIoWavenetSpecError::WeightBlobMismatch);
}

TEST_CASE("WaveNet shared spec authenticates the serialized head scale",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.weights.back() = 0.5f;
    DawnSharedIoWavenetProgramSpec spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 1,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    CHECK(validate_dawn_shared_io_wavenet_spec(spec).error ==
          DawnSharedIoWavenetSpecError::InvalidScale);

    spec.head_scale = std::numeric_limits<float>::quiet_NaN();
    fixture.weights.back() = spec.head_scale;
    CHECK(validate_dawn_shared_io_wavenet_spec(spec).error ==
          DawnSharedIoWavenetSpecError::InvalidScale);
}

TEST_CASE("WaveNet shared spec rejects non-mono conditioning and broken layer chains",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.layer.condition_size = 2;
    const DawnSharedIoWavenetProgramSpec condition_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    auto result = validate_dawn_shared_io_wavenet_spec(condition_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidCondition);

    fixture.layer.condition_size = 1;
    fixture.layer.input_size = 2;
    const DawnSharedIoWavenetProgramSpec chain_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    result = validate_dawn_shared_io_wavenet_spec(chain_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidChain);
}

TEST_CASE("WaveNet shared spec rejects zero stream instances and dilation",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.dilations[0] = 0;
    const DawnSharedIoWavenetProgramSpec dilation_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 2,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    auto result = validate_dawn_shared_io_wavenet_spec(dilation_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidDilation);

    fixture.dilations[0] = 1;
    const DawnSharedIoWavenetProgramSpec instance_spec{
        .block_size = 32,
        .head_scale = 1.0f,
        .stream_instances = 0,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    result = validate_dawn_shared_io_wavenet_spec(instance_spec);
    CHECK(result.error == DawnSharedIoWavenetSpecError::InvalidShape);
}

TEST_CASE("WaveNet shared spec rejects invalid flags and non-mono final heads",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    const auto validate = [&] {
        return validate_dawn_shared_io_wavenet_spec({
            .block_size = 32,
            .head_scale = 1.0f,
            .stream_instances = 1,
            .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
            .weights = fixture.weights,
        });
    };

    fixture.layer.gated = 2;
    CHECK(validate().error == DawnSharedIoWavenetSpecError::InvalidLayer);

    fixture.layer.gated = 1;
    fixture.layer.head_bias = 2;
    CHECK(validate().error == DawnSharedIoWavenetSpecError::InvalidLayer);

    fixture.layer.head_bias = 1;
    fixture.layer.head_size = 2;
    CHECK(validate().error == DawnSharedIoWavenetSpecError::InvalidChain);
}

TEST_CASE("WaveNet shared spec rejects history and resource-size overflow",
          "[gpu_audio][shared_io][wavenet]") {
    const std::array<float, 1> weights{0.0f};
    const std::uint32_t dilation = 2;
    DawnSharedIoWavenetLayerSpec layer{
        .input_size = 1,
        .condition_size = 1,
        .channels = 1,
        .kernel = std::numeric_limits<std::uint32_t>::max(),
        .head_size = 1,
        .gated = 0,
        .head_bias = 0,
        .dilations = std::span<const std::uint32_t>(&dilation, 1),
    };
    const auto validate = [&] {
        return validate_dawn_shared_io_wavenet_spec({
            .block_size = 32,
            .head_scale = 1.0f,
            .stream_instances = 1,
            .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&layer, 1),
            .weights = weights,
        });
    };

    CHECK(validate().error == DawnSharedIoWavenetSpecError::HistoryOverflow);

    layer.kernel = 1;
    layer.channels = 2;
    layer.head_size = std::numeric_limits<std::uint32_t>::max();
    CHECK(validate().error == DawnSharedIoWavenetSpecError::ResourceOverflow);
}

#if defined(PULP_GPU_AUDIO_WAVENET_RUNTIME)
TEST_CASE("authenticated Dawn WaveNet preserves causal history across rotating slots",
          "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.dilations = {1, 1};
    fixture.layer.channels = 1;
    fixture.layer.kernel = 2;
    fixture.layer.head_size = 1;
    fixture.layer.gated = 0;
    fixture.layer.head_bias = 0;
    fixture.layer.dilations = std::span<const std::uint32_t>(fixture.dilations.data(), 1);
    // rechannel=1; conv reads 0.5 * the immediately previous sample and
    // ignores the current sample; the residual path is disabled; head=1.
    fixture.weights = {1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 2,
        .head_scale = 1.0f,
        .stream_instances = 1,
        .arrays = std::span<const DawnSharedIoWavenetLayerSpec>(&fixture.layer, 1),
        .weights = fixture.weights,
    };
    auto created = DawnSharedIoProvider::create({});
    INFO(created.reason);
    REQUIRE(created.provider);
    auto program = created.provider->make_wavenet_program(spec);
    REQUIRE(program);
    SharedIoArena arena;
    REQUIRE(arena.prepare(*created.provider,
                          {.slots = 2,
                           .input_bytes_per_slot = 2 * sizeof(float),
                           .output_bytes_per_slot = 2 * sizeof(float)},
                          std::move(program)));

    const auto submit = [&](std::uint64_t sequence, std::array<float, 2> input) {
        auto write = arena.grant_write(sequence);
        REQUIRE(write);
        std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
        REQUIRE(arena.publish_written({write->token}));
        REQUIRE(arena.submit(write->token));
    };
    const auto collect = [&](std::uint64_t sequence) {
        std::optional<SharedIoArena::OutputLease> output;
        for (int i = 0; i < 200 && !output; ++i) {
            created.provider->poll();
            arena.drain_completions();
            output = arena.acquire_output(arena.preparation_epoch(), sequence);
            if (!output)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(output);
        const auto* values = reinterpret_cast<const float*>(output->bytes.data());
        const std::array<float, 2> result{values[0], values[1]};
        const auto token = output->token;
        output.reset();
        REQUIRE(arena.release_output({token}));
        return result;
    };

    // Submit two blocks before either completes so the provider must rotate
    // physical slots while advancing one causal stream in queue order.
    submit(1, {1.0f, 2.0f});
    submit(2, {3.0f, 4.0f});
    const auto first = collect(1);
    const auto second = collect(2);
    submit(3, {5.0f, 6.0f}); // wrap to the first physical slot
    const auto third = collect(3);

    // A gap would advance the one causal state with the wrong logical block.
    // Reject it before queue submission, then prove the missing sequence can
    // still advance the stream without the rejected input contaminating it.
    auto skipped = arena.grant_write(5);
    REQUIRE(skipped);
    const std::array<float, 2> skipped_input{99.0f, 100.0f};
    std::copy(skipped_input.begin(), skipped_input.end(),
              reinterpret_cast<float*>(skipped->bytes.data()));
    REQUIRE(arena.publish_written({skipped->token}));
    CHECK_FALSE(arena.submit(skipped->token));
    submit(4, {7.0f, 8.0f});
    const auto fourth = collect(4);

    const std::array<float, 8> actual{first[0], first[1], second[0], second[1],
                                      third[0], third[1], fourth[0], fourth[1]};
    const std::array<float, 8> expected{0.0f,
                                        std::tanh(0.5f),
                                        std::tanh(1.0f),
                                        std::tanh(1.5f),
                                        std::tanh(2.0f),
                                        std::tanh(2.5f),
                                        std::tanh(3.0f),
                                        std::tanh(3.5f)};
    for (std::size_t index = 0; index < actual.size(); ++index)
        CHECK(actual[index] == Catch::Approx(expected[index]).margin(1.0e-5));
    REQUIRE(arena.release());
}

TEST_CASE("authenticated Dawn WaveNet matches a two-array multi-dilation CPU oracle",
          "[gpu_audio][shared_io][wavenet]") {
    const std::array<std::uint32_t, 2> first_dilations{1, 2};
    const std::array<std::uint32_t, 1> second_dilations{1};
    const std::array<DawnSharedIoWavenetLayerSpec, 2> arrays{{
        {.input_size = 1,
         .condition_size = 1,
         .channels = 1,
         .kernel = 2,
         .head_size = 1,
         .gated = 0,
         .head_bias = 0,
         .dilations = first_dilations},
        {.input_size = 1,
         .condition_size = 1,
         .channels = 1,
         .kernel = 2,
         .head_size = 1,
         .gated = 0,
         .head_bias = 1,
         .dilations = second_dilations},
    }};
    // array 0: rechannel; two layers of conv W+bias, mixin W, residual W+bias;
    // head W. array 1: the same with one layer and a biased head. Final scalar
    // is the serialized head scale.
    const std::vector<float> weights{
        1.0f,  0.2f, 0.4f, 0.1f,  0.05f, 0.3f,  -0.02f, -0.1f, 0.25f, -0.03f, 0.04f,  0.2f,
        0.01f, 0.7f, 0.8f, 0.15f, 0.35f, 0.02f, -0.05f, 0.1f,  0.03f, 0.6f,   -0.04f, 0.5f,
    };
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 3,
        .head_scale = 0.5f,
        .stream_instances = 1,
        .arrays = arrays,
        .weights = weights,
    };
    REQUIRE(validate_dawn_shared_io_wavenet_spec(spec).accepted());

    auto created = DawnSharedIoProvider::create({});
    INFO(created.reason);
    REQUIRE(created.provider);
    auto program = created.provider->make_wavenet_program(spec);
    REQUIRE(program);
    SharedIoArena arena;
    REQUIRE(arena.prepare(*created.provider,
                          {.slots = 2,
                           .input_bytes_per_slot = 3 * sizeof(float),
                           .output_bytes_per_slot = 3 * sizeof(float)},
                          std::move(program)));

    TinyReference reference;
    const std::array<std::array<float, 3>, 3> inputs{{
        {0.2f, -0.1f, 0.3f},
        {0.4f, 0.05f, -0.2f},
        {0.1f, 0.25f, -0.35f},
    }};
    std::array<std::array<float, 3>, 3> expected{};
    for (std::size_t block = 0; block < inputs.size(); ++block)
        for (std::size_t sample = 0; sample < inputs[block].size(); ++sample)
            expected[block][sample] = reference.process(inputs[block][sample]);

    const auto submit = [&](std::uint64_t sequence, const std::array<float, 3>& input) {
        auto write = arena.grant_write(sequence);
        REQUIRE(write);
        std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
        REQUIRE(arena.publish_written({write->token}));
        REQUIRE(arena.submit(write->token));
    };
    const auto collect = [&](std::uint64_t sequence) {
        std::optional<SharedIoArena::OutputLease> output;
        for (int i = 0; i < 200 && !output; ++i) {
            created.provider->poll();
            arena.drain_completions();
            output = arena.acquire_output(arena.preparation_epoch(), sequence);
            if (!output)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(output);
        const auto* values = reinterpret_cast<const float*>(output->bytes.data());
        const std::array<float, 3> result{values[0], values[1], values[2]};
        const auto token = output->token;
        output.reset();
        REQUIRE(arena.release_output({token}));
        return result;
    };

    submit(1, inputs[0]);
    submit(2, inputs[1]);
    const auto first = collect(1);
    const auto second = collect(2);
    submit(3, inputs[2]);
    const auto third = collect(3);
    const std::array<std::array<float, 3>, 3> actual{first, second, third};
    for (std::size_t block = 0; block < actual.size(); ++block)
        for (std::size_t sample = 0; sample < actual[block].size(); ++sample)
            CHECK(actual[block][sample] == Catch::Approx(expected[block][sample]).margin(1.0e-5));
    REQUIRE(arena.release());
}

TEST_CASE("authenticated Dawn WaveNet executes channel matrices and cross-array head seeding",
          "[gpu_audio][shared_io][wavenet]") {
    const std::uint32_t dilation = 1;
    const std::array<DawnSharedIoWavenetLayerSpec, 2> arrays{{
        {.input_size = 1,
         .condition_size = 1,
         .channels = 2,
         .kernel = 1,
         .head_size = 2,
         .gated = 0,
         .head_bias = 0,
         .dilations = std::span<const std::uint32_t>(&dilation, 1)},
        {.input_size = 2,
         .condition_size = 1,
         .channels = 2,
         .kernel = 1,
         .head_size = 1,
         .gated = 0,
         .head_bias = 0,
         .dilations = std::span<const std::uint32_t>(&dilation, 1)},
    }};
    const std::vector<float> weights{
        // Array 0: 2x1 rechannel; one fixed-bias activation layer; 2x2 head.
        1.0f,
        2.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.1f,
        -0.2f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        2.0f,
        3.0f,
        -1.0f,
        // Array 1: non-identity 2x2 rechannel; identity convolution; scalar head.
        0.5f,
        -0.25f,
        1.5f,
        0.75f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.25f,
        -0.5f,
        2.0f,
    };
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 3,
        .head_scale = 2.0f,
        .stream_instances = 1,
        .arrays = arrays,
        .weights = weights,
    };
    REQUIRE(validate_dawn_shared_io_wavenet_spec(spec).accepted());

    auto created = DawnSharedIoProvider::create({});
    INFO(created.reason);
    REQUIRE(created.provider);
    auto program = created.provider->make_wavenet_program(spec);
    REQUIRE(program);
    SharedIoArena arena;
    REQUIRE(arena.prepare(*created.provider,
                          {.slots = 1,
                           .input_bytes_per_slot = 3 * sizeof(float),
                           .output_bytes_per_slot = 3 * sizeof(float)},
                          std::move(program)));

    const std::array<float, 3> input{0.2f, -0.1f, 0.3f};
    auto write = arena.grant_write(1);
    REQUIRE(write);
    std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
    REQUIRE(arena.publish_written({write->token}));
    REQUIRE(arena.submit(write->token));

    std::optional<SharedIoArena::OutputLease> output;
    for (int i = 0; i < 200 && !output; ++i) {
        created.provider->poll();
        arena.drain_completions();
        output = arena.acquire_output(arena.preparation_epoch(), 1);
        if (!output)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(output);
    const auto a0 = std::tanh(0.1f);
    const auto a1 = std::tanh(-0.2f);
    const auto seeded0 = a0 + 2.0f * a1;
    const auto seeded1 = 3.0f * a0 - a1;
    const auto* actual = reinterpret_cast<const float*>(output->bytes.data());
    for (std::size_t frame = 0; frame < input.size(); ++frame) {
        const auto array1_activation = std::tanh(3.0f * input[frame]);
        const auto expected = 2.0f * (0.25f * seeded0 - 0.5f * (seeded1 + array1_activation));
        CHECK(actual[frame] == Catch::Approx(expected).margin(1.0e-5));
    }
    const auto token = output->token;
    output.reset();
    REQUIRE(arena.release_output({token}));
    REQUIRE(arena.release());
}
#endif
