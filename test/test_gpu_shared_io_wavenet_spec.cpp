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
    if (!created.provider)
        SKIP("Dawn/Metal provider unavailable on this host");
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
#endif
