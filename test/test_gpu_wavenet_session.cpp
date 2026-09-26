#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <span>
#include <thread>

using namespace pulp::gpu_audio;

namespace {
struct Fixture {
    std::uint32_t dilation = 1;
    GpuWaveNetLayerDescriptor layer{
        .input_size = 1,
        .condition_size = 1,
        .channels = 1,
        .kernel = 2,
        .head_size = 1,
        .dilation = dilation,
        .gated = false,
        .head_bias = false,
        .tanh_activation = true,
    };
    std::array<float, 9> weights{1.0f, 0.5f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 1.0f};

    GpuWaveNetDescriptor descriptor() const noexcept {
        return {.block_size = 2,
                .sample_rate = 48'000,
                .stream_instances = 1,
                .head_scale = 1.0f,
                .layers = {&layer, 1},
                .weight_count = weights.size()};
    }
};
} // namespace

TEST_CASE("public WaveNet session rejects a weight span that disagrees with the descriptor",
          "[gpu_audio][wavenet][session]") {
    Fixture fixture;
    const auto descriptor = fixture.descriptor();
    const auto result = GpuWaveNetSession::create(
        {.descriptor = descriptor,
         .weights = std::span<const float>(fixture.weights.data(), fixture.weights.size() - 1),
         .slots = 2});
    CHECK_FALSE(result);
    CHECK(result.error == GpuWaveNetSessionError::InvalidWeights);
}

TEST_CASE("public WaveNet session reports provider capability without exposing detail types",
          "[gpu_audio][wavenet][session]") {
    Fixture fixture;
    const auto result = GpuWaveNetSession::create({.descriptor = fixture.descriptor(),
                                                   .weights = fixture.weights,
                                                   .slots = 2});

    if (!result.session) {
        CHECK(result.error == GpuWaveNetSessionError::ProviderUnavailable);
        return;
    }
    REQUIRE(result);
    auto& session = *result.session;
    CHECK(session.prepared());
    CHECK(session.block_size() == fixture.descriptor().block_size);

    const std::array<float, 2> input{1.0f, 2.0f};
    std::array<float, 2> output{0.0f, 0.0f};
    REQUIRE(session.submit_block(input, 1));

    std::optional<GpuWaveNetBlockResult> completion;
    for (int attempt = 0; attempt < 200 && !completion; ++attempt) {
        session.service(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()));
        completion = session.receive(output);
        if (!completion)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(completion);
    REQUIRE(completion->status == GpuWaveNetBlockStatus::GpuDelivered);
    CHECK(completion->sequence == 1);
    CHECK(output[0] == Catch::Approx(0.0f).margin(1.0e-5));
    CHECK(output[1] == Catch::Approx(std::tanh(0.5f)).margin(1.0e-5));
    REQUIRE(session.release());
}
