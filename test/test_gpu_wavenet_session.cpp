#include <pulp/gpu_audio/gpu_wavenet.hpp>
#include "detail/nam_tcn_artifact.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <iostream>
#include <thread>
#include <vector>

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
    std::array<float, 9> weights{1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};

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
    const auto result = GpuWaveNetSession::create(
        {.descriptor = fixture.descriptor(), .weights = fixture.weights, .slots = 2});

    if (!result.session) {
        CHECK(result.error == GpuWaveNetSessionError::ProviderUnavailable);
        return;
    }
    REQUIRE(result);
    auto& session = *result.session;
    CHECK(session.prepared());
    CHECK(session.block_size() == fixture.descriptor().block_size);
    CHECK(session.completion_policy() == GpuWaveNetCompletionPolicy::ProcessEvents);
    CHECK(session.completion_policy_supported());

    const std::array<float, 2> input{1.0f, 2.0f};
    std::array<float, 2> output{0.0f, 0.0f};
    REQUIRE(session.submit_block(input, 1));

    std::optional<GpuWaveNetBlockResult> completion;
    for (int attempt = 0; attempt < 200 && !completion; ++attempt) {
        session.service(
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
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

TEST_CASE("WaveNet completion policy is explicit non-realtime configuration",
          "[gpu_audio][wavenet][completion]") {
    Fixture fixture;
    const auto result =
        GpuWaveNetSession::create({.descriptor = fixture.descriptor(),
                                   .weights = fixture.weights,
                                   .slots = 2,
                                   .completion_policy = GpuWaveNetCompletionPolicy::TimedWaitAny,
                                   .completion_wait_ns = 500'000});
    if (!result.session) {
        CHECK(result.error == GpuWaveNetSessionError::ProviderUnavailable);
        return;
    }
    CHECK(result.session->prepared());
    CHECK(result.session->completion_policy() == GpuWaveNetCompletionPolicy::TimedWaitAny);
    CHECK(result.session->completion_policy_supported());
    const std::array<float, 2> input{1.0f, 2.0f};
    std::array<float, 2> output{0.0f, 0.0f};
    REQUIRE(result.session->submit_block(input, 1));
    // This test verifies that the explicit non-realtime policy is accepted and
    // serviced. It is not a realtime deadline test. Merge-group runners can be
    // occupied by unrelated jobs, so give the asynchronous Dawn callback a
    // bounded but deliberately generous observation window.
    const auto wait_for_completion = [&](std::uint64_t sequence) {
        const auto deadline =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()) +
            250'000'000;
        std::optional<GpuWaveNetBlockResult> completion;
        for (int attempt = 0; attempt < 500 && !completion; ++attempt) {
            result.session->service_until(deadline);
            completion = result.session->receive(output);
        }
        CHECK(completion.has_value());
        if (completion)
            CHECK(completion->sequence == sequence);
    };
    wait_for_completion(1);
    REQUIRE(result.session->submit_block(input, 2));
    wait_for_completion(2);
    CHECK(result.session->release());
}

TEST_CASE("public WaveNet session accepts a reusable slot-depth matrix",
          "[gpu_audio][wavenet][session][slots]") {
    Fixture fixture;
    const std::array<std::uint32_t, 3> slot_depths{2, 4, 8};
    const std::array<float, 2> input{1.0f, 2.0f};

    for (const auto slots : slot_depths) {
        const auto result = GpuWaveNetSession::create(
            {.descriptor = fixture.descriptor(), .weights = fixture.weights, .slots = slots});
        if (!result.session) {
            CHECK(result.error == GpuWaveNetSessionError::ProviderUnavailable);
            return;
        }
        REQUIRE(result);
        auto& session = *result.session;
        CHECK(session.completion_policy() == GpuWaveNetCompletionPolicy::ProcessEvents);
        CHECK(session.completion_policy_supported());

        for (std::uint64_t sequence = 1; sequence <= slots; ++sequence)
            REQUIRE(session.submit_block(input, sequence));

        std::array<bool, 8> seen{};
        std::array<float, 2> output{};
        std::size_t received = 0;
        for (int attempt = 0; attempt < 500 && received < slots; ++attempt) {
            session.service(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count()));
            if (const auto completion = session.receive(output)) {
                REQUIRE(completion->status == GpuWaveNetBlockStatus::GpuDelivered);
                REQUIRE(completion->sequence >= 1);
                REQUIRE(completion->sequence <= slots);
                const auto index = static_cast<std::size_t>(completion->sequence - 1);
                CHECK_FALSE(seen[index]);
                seen[index] = true;
                ++received;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        REQUIRE(received == slots);
        for (std::size_t index = 0; index < slots; ++index)
            CHECK(seen[index]);
        REQUIRE(session.release());
    }
}

#if defined(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF)
#ifndef PULP_GPU_AUDIO_NAM_FIXTURE
#define PULP_GPU_AUDIO_NAM_FIXTURE "test/fixtures/neural/example.nam"
#endif

TEST_CASE("named WaveNet fixture remains causal across transport slot reuse",
          "[gpu_audio][wavenet][session][named-causal]") {
    if (std::getenv("PULP_RUN_NAMED_CAUSAL_CAMPAIGN") == nullptr)
        SKIP("manual authenticated 100000-block campaign is disabled by default");
    pulp::gpu_audio::detail::NamTcnArtifact oracle;
    std::string oracle_error;
    REQUIRE(oracle.load(PULP_GPU_AUDIO_NAM_FIXTURE, &oracle_error));
    std::vector<GpuWaveNetLayerDescriptor> layers;
    std::vector<std::vector<std::uint32_t>> dilations;
    std::vector<float> weights;
    GpuWaveNetDescriptor descriptor;
    REQUIRE(oracle.make_gpu_wavenet_descriptor(64, descriptor, layers, dilations, weights));
    REQUIRE(descriptor.sample_rate == 48000);
    const auto input_for = [](std::uint64_t sequence) {
        std::array<float, 64> input{};
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            input[frame] = 0.17f * std::sin(0.013f * static_cast<float>(sequence * 64 + frame)) +
                           0.07f * std::cos(0.031f * static_cast<float>(sequence * 64 + frame));
        return input;
    };
    const auto run = [&](std::uint32_t slots, std::uint32_t batch_size) {
        const auto result = GpuWaveNetSession::create(
            {.descriptor = descriptor, .weights = weights, .slots = slots});
        if (!result.session) {
            SKIP("authenticated provider unavailable; named causal regression skipped (error="
                 << static_cast<int>(result.error) << ")");
            return std::vector<std::array<float, 64>>{};
        }
        auto& session = *result.session;
        constexpr std::uint64_t kBlocks = 100000;
        std::vector<std::array<float, 64>> outputs(kBlocks);
        for (std::uint64_t base = 1; base <= kBlocks; base += batch_size) {
            const auto count = std::min<std::uint32_t>(
                batch_size, static_cast<std::uint32_t>(kBlocks + 1u - base));
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                const auto input = input_for(base + offset);
                REQUIRE(session.submit_block(input, base + offset));
            }
            std::uint32_t retired = 0;
            for (int attempt = 0; attempt < 2000 && retired < count; ++attempt) {
                session.service(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()));
                std::array<float, 64> output{};
                if (const auto completion = session.receive(output)) {
                    REQUIRE(completion->status == GpuWaveNetBlockStatus::GpuDelivered);
                    REQUIRE(completion->sequence >= base);
                    REQUIRE(completion->sequence < base + count);
                    outputs[completion->sequence - 1] = output;
                    ++retired;
                } else
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            REQUIRE(retired == count);
        }
        REQUIRE(session.release());
        return outputs;
    };
    std::vector<std::array<float, 64>> expected(100000);
    for (std::uint64_t sequence = 1; sequence <= expected.size(); ++sequence) {
        const auto input = input_for(sequence);
        oracle.process(input.data(), expected[sequence - 1].data(), input.size());
    }
    const auto reference = run(2, 1);
    if (reference.empty()) {
        SKIP("authenticated provider unavailable; named causal regression skipped");
        return;
    }
    const auto batched = run(4, 4);
    REQUIRE_FALSE(batched.empty());
    double max_residual = 0.0;
    for (std::size_t block = 0; block < reference.size(); ++block)
        for (std::size_t frame = 0; frame < reference[block].size(); ++frame) {
            max_residual =
                std::max(max_residual, std::abs(static_cast<double>(reference[block][frame]) -
                                                expected[block][frame]));
            max_residual =
                std::max(max_residual, std::abs(static_cast<double>(batched[block][frame]) -
                                                expected[block][frame]));
            CHECK(reference[block][frame] == Catch::Approx(expected[block][frame]).margin(2.0e-5));
            CHECK(batched[block][frame] == Catch::Approx(expected[block][frame]).margin(2.0e-5));
        }
    std::cout
        << "{\"schema\":\"pulp.gpu-audio.wavenet.named-causal.v1\",\"status\":\"passed\",\"fixture_"
           "sha256\":\"66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d\","
           "\"blocks\":100000,\"reference_slots\":2,\"batched_slots\":4,\"max_cpu_residual\":"
        << max_residual << "}\n";
}

#endif
