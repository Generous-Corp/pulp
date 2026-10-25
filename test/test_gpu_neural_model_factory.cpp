// The public neural model factory, used the way an SDK consumer would: only
// installed headers, a consumer-owned kernel, a node and a transport.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_neural_model.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

#include <array>
#include <cmath>
#include <string_view>
#include <vector>

using namespace pulp;
using namespace pulp::gpu_audio;

namespace {

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlock = 64;
constexpr std::uint32_t kLead = 2;
constexpr std::string_view kSha =
    "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";

/// A consumer's stateful model: a one-pole low-pass per channel.
struct OnePole {
    float coefficient = 0.9f;
    float memory = 0.0f;
    float step(float x) noexcept {
        memory = coefficient * memory + (1.0f - coefficient) * x;
        return memory;
    }
    static GpuNeuralCpuKernel kernel(OnePole& self) {
        return {&self,
                [](void* state) noexcept {
                    static_cast<OnePole*>(state)->memory = 0.0f;
                    return true;
                },
                [](void* state, const float* in, float* out, std::uint32_t frames) noexcept {
                    auto& pole = *static_cast<OnePole*>(state);
                    for (std::uint32_t i = 0; i < frames; ++i)
                        out[i] = pole.step(in[i]);
                },
                [](void* state) noexcept { static_cast<OnePole*>(state)->memory = 0.0f; }};
    }
};

float stimulus(std::uint32_t ch, std::uint64_t frame) {
    return 0.5f * std::sin(0.01f * static_cast<float>(frame) * static_cast<float>(ch + 1));
}

GpuNeuralModelRequest request_for(NeuralFamily family,
                                  std::span<const GpuNeuralCpuKernel> kernels) {
    GpuNeuralModelRequest request;
    request.family = family;
    request.manifest = {.model_id = "consumer.model",
                        .architecture = family == NeuralFamily::Tcn ? "tcn.causal" : "consumer",
                        .model_version = "1",
                        .artifact_id = "consumer.weights",
                        .artifact_sha256 = kSha,
                        .license = "MIT",
                        .runtime = "consumer.runtime.v1",
                        .artifact_size_bytes = 1024,
                        .sample_rate_hz = 48000,
                        .redistributable = true};
    request.shape = {
        .tcn_kernel_size = 3, .tcn_dilation = 2, .recurrent_hidden_size = 8, .ssm_state_size = 16};
    request.block_size = kBlock;
    request.channel_kernels = kernels;
    return request;
}

} // namespace

TEST_CASE("a consumer builds and runs a neural model through the public factory",
          "[gpu_audio][neural][factory][public]") {
    const auto family = GENERATE(NeuralFamily::Tcn, NeuralFamily::Ssm, NeuralFamily::Lstm,
                                 NeuralFamily::Gru, NeuralFamily::WaveNet, NeuralFamily::Custom);
    CAPTURE(static_cast<int>(family));
    std::array<OnePole, kChannels> poles;
    std::array<GpuNeuralCpuKernel, kChannels> kernels{OnePole::kernel(poles[0]),
                                                      OnePole::kernel(poles[1])};
    auto result = make_gpu_neural_model(request_for(family, kernels));
    REQUIRE(result);
    GpuNeuralRealtimeNode node({.family = family,
                                .model = result.model,
                                .channels = kChannels,
                                .lead_blocks = kLead,
                                .capacity = kLead + 3});
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {}));

    // The node's output is the consumer's model, delayed by the lead.
    std::array<OnePole, kChannels> reference;
    std::array<std::vector<float>, kChannels> expected;
    float worst = 0.0f;
    std::uint64_t frame = 0;
    for (int b = 0; b < 32; ++b) {
        std::array<std::vector<float>, kChannels> in, out;
        std::array<const float*, kChannels> in_ptrs{};
        std::array<float*, kChannels> out_ptrs{};
        for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
            in[ch].resize(kBlock);
            out[ch].assign(kBlock, 0.0f);
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                in[ch][i] = stimulus(ch, frame + i);
                expected[ch].push_back(reference[ch].step(in[ch][i]));
            }
            in_ptrs[ch] = in[ch].data();
            out_ptrs[ch] = out[ch].data();
        }
        audio::BufferView<const float> input(in_ptrs.data(), kChannels, kBlock);
        audio::BufferView<float> output(out_ptrs.data(), kChannels, kBlock);
        transport.process(input, output, kBlock);
        transport.pump();
        for (std::uint32_t ch = 0; ch < kChannels; ++ch)
            for (std::uint32_t i = 0; i < kBlock; ++i) {
                const auto at = frame + i;
                const float want = at < kLead * kBlock ? 0.0f : expected[ch][at - kLead * kBlock];
                worst = std::max(worst, std::abs(out[ch][i] - want));
            }
        frame += kBlock;
    }
    CHECK(worst == 0.0f);
    const auto status = transport.status_snapshot();
    CHECK(validate_gpu_audio_status(status).accepted());
    CHECK(validate_gpu_audio_status(node.status_snapshot()).accepted());
    CHECK(status.provider == GpuAudioProvider::Cpu);
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
}

TEST_CASE("the public factory reports why it refused a model",
          "[gpu_audio][neural][factory][public]") {
    std::array<OnePole, kChannels> poles;
    std::array<GpuNeuralCpuKernel, kChannels> kernels{OnePole::kernel(poles[0]),
                                                      OnePole::kernel(poles[1])};
    GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                    .condition_size = 1,
                                    .channels = 1,
                                    .kernel = 2,
                                    .head_size = 1,
                                    .dilation = 1,
                                    .tanh_activation = true};
    std::array<float, 9> weights{};
    const GpuWaveNetSession::Config session{.descriptor = {.block_size = kBlock,
                                                           .sample_rate = 48000,
                                                           .stream_instances = 1,
                                                           .layers = {&layer, 1},
                                                           .weight_count = weights.size()},
                                            .weights = weights};
    const auto mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    CAPTURE(mode);
    auto request = request_for(NeuralFamily::Lstm, kernels);
    auto expected = GpuNeuralModelError::None;
    std::array<GpuNeuralCpuKernel, kChannels> broken = kernels;
    switch (mode) {
    case 0: // positive control
        break;
    case 1:
        request.manifest.artifact_sha256 = "not-a-hash";
        expected = GpuNeuralModelError::InvalidManifest;
        break;
    case 2:
        request.manifest.redistributable = false; // shipped, so refused
        expected = GpuNeuralModelError::InvalidManifest;
        break;
    case 3:
        request.shape.recurrent_hidden_size = 0;
        expected = GpuNeuralModelError::ShapeMismatch;
        break;
    case 4:
        broken[1].process = nullptr;
        request.channel_kernels = broken;
        expected = GpuNeuralModelError::MissingKernels;
        break;
    case 5:
        request.wavenet_gpu = &session; // LSTM has no GPU kernel
        expected = GpuNeuralModelError::UnsupportedBackend;
        break;
    case 6:
        request.family = NeuralFamily::Unknown;
        expected = GpuNeuralModelError::UnsupportedFamily;
        break;
    case 7:
        request.block_size = 0;
        expected = GpuNeuralModelError::InvalidBlockSize;
        break;
    }
    const auto result = make_gpu_neural_model(request);
    CHECK(result.error == expected);
    CHECK(static_cast<bool>(result) == (mode == 0));
}

TEST_CASE("a WaveNet model with a GPU session reports its real path",
          "[gpu_audio][neural][factory][public]") {
    std::array<OnePole, kChannels> poles;
    std::array<GpuNeuralCpuKernel, kChannels> kernels{OnePole::kernel(poles[0]),
                                                      OnePole::kernel(poles[1])};
    GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                    .condition_size = 1,
                                    .channels = 1,
                                    .kernel = 2,
                                    .head_size = 1,
                                    .dilation = 1,
                                    .tanh_activation = true};
    std::array<float, 9> weights{};
    // The serialized blob ends with the head scale, which the provider checks
    // against the descriptor's (1.0). An all-zero blob is refused on every
    // host, which would leave the probe below proving nothing.
    weights.back() = 1.0f;
    const GpuWaveNetSession::Config session{.descriptor = {.block_size = kBlock,
                                                           .sample_rate = 48000,
                                                           .stream_instances = 1,
                                                           .layers = {&layer, 1},
                                                           .weight_count = weights.size()},
                                            .weights = weights};
    INFO("descriptor error " << static_cast<int>(
             validate_gpu_wavenet_descriptor(session.descriptor).error));
    REQUIRE(validate_gpu_wavenet_descriptor(session.descriptor).accepted());
    // The descriptor check counts weights but does not read them, and a
    // build without a provider refuses create() before it would; so check
    // the blob's trailing head scale here, on every host.
    REQUIRE(session.weights.size() == session.descriptor.weight_count);
    REQUIRE(session.weights.back() == session.descriptor.head_scale);
    auto request = request_for(NeuralFamily::WaveNet, kernels);
    request.wavenet_gpu = &session;
    auto result = make_gpu_neural_model(request);
    REQUIRE(result);
    GpuNeuralRealtimeNode node({.family = NeuralFamily::WaveNet,
                                .model = result.model,
                                .channels = kChannels,
                                .lead_blocks = kLead,
                                .capacity = kLead + 3});
    REQUIRE(node.prepare());
    const auto status = node.status_snapshot();
    CHECK(validate_gpu_audio_status(status).accepted());
    CHECK(status.requested_engine == GpuAudioEngine::Gpu);
    if (node.gpu_kernel_available()) {
        // An authenticated provider on this host: the GPU path is live.
        CHECK(status.provider == GpuAudioProvider::Dawn);
        CHECK(status.provider_state == GpuAudioProviderState::Ready);
    } else {
        // Degraded is right only on a host that cannot create the session. If
        // the session creates here, the factory's GPU path is broken and this
        // branch must not pass on its own.
        const auto probe = GpuWaveNetSession::create(session);
        CHECK_FALSE(static_cast<bool>(probe));
        // ...and it must fail for want of a provider, not because this test's
        // session is malformed, or the probe would prove nothing.
        INFO("GpuWaveNetSession::create error " << static_cast<int>(probe.error));
        CHECK((probe.error == GpuWaveNetSessionError::ProviderUnavailable ||
               probe.error == GpuWaveNetSessionError::ProgramUnavailable ||
               probe.error == GpuWaveNetSessionError::PreparationFailed));
        // No provider here: the CPU model plays, reported Degraded with why.
        CHECK(status.provider == GpuAudioProvider::Cpu);
        CHECK(status.provider_state == GpuAudioProviderState::Degraded);
        CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderFailure);
    }
}
