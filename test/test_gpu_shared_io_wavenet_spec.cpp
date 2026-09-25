#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/dawn_shared_io_wavenet_spec.hpp"
#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_arena.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>
#include <thread>
#include <cmath>

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
    auto program = DawnSharedIoWavenetProgram::create(spec);
    REQUIRE(program);
    CHECK(program->weight_count() == fixture.weights.size());
    CHECK(program->history_bytes() != 0);
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

TEST_CASE("authenticated Dawn WaveNet executes one mono block", "[gpu_audio][shared_io][wavenet]") {
    Fixture fixture;
    fixture.dilations = {1, 1};
    fixture.layer.channels = 1;
    fixture.layer.kernel = 1;
    fixture.layer.head_size = 1;
    fixture.layer.gated = 0;
    fixture.layer.head_bias = 0;
    fixture.layer.dilations = std::span<const std::uint32_t>(fixture.dilations.data(), 1);
    fixture.weights = {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    const DawnSharedIoWavenetProgramSpec spec{
        .block_size = 4,
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
                          {.slots = 1, .input_bytes_per_slot = 4 * sizeof(float),
                           .output_bytes_per_slot = 4 * sizeof(float)}, std::move(program)));
    auto write = arena.grant_write(1);
    REQUIRE(write);
    const float input[] = {0.0f, 0.5f, -1.0f, 2.0f};
    std::copy(std::begin(input), std::end(input), reinterpret_cast<float*>(write->bytes.data()));
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
    const auto* actual = reinterpret_cast<const float*>(output->bytes.data());
    for (std::size_t i = 0; i < std::size(input); ++i)
        CHECK(actual[i] == Catch::Approx(std::tanh(input[i])).margin(1.0e-5));
    const auto token = output->token;
    output.reset();
    REQUIRE(arena.release_output({token}));
    REQUIRE(arena.release());
}
