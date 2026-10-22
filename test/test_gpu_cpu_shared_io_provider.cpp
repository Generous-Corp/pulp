#include "detail/cpu_shared_io_provider.hpp"
#include "detail/shared_io_program_session.hpp"
#include "detail/shared_io_stamped_bridge.hpp"

#include <pulp/gpu_audio/gpu_audio_capability.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace {

using pulp::gpu_audio::GpuAudioProvider;
using pulp::gpu_audio::detail::CpuSharedIoArenaProvider;
using pulp::gpu_audio::detail::CpuSharedIoConvolutionSessionCreateResult;
using pulp::gpu_audio::detail::create_cpu_shared_io_convolution_session;
using pulp::gpu_audio::detail::SharedIoArena;
using pulp::gpu_audio::detail::SharedIoArenaProvider;
using pulp::gpu_audio::detail::SharedIoConvolutionSession;
using pulp::gpu_audio::detail::SharedIoProgramSession;
using pulp::gpu_audio::detail::SharedIoStampedBridge;
using pulp::gpu_audio::detail::SharedIoTerminalStatus;
using pulp::gpu_audio::detail::WavenetLayerSpec;
using pulp::gpu_audio::detail::WavenetProgramSpec;
using SlotToken = SharedIoArenaProvider::SlotToken;
using StorageKind = SharedIoArenaProvider::StorageKind;

constexpr std::uint32_t kFrames = 16;

struct TerminalLog {
    std::vector<std::pair<SlotToken, SharedIoTerminalStatus>> entries;

    SharedIoArena::CompletionObserver observer() noexcept {
        return {this,
                [](void* context, const SlotToken& token, SharedIoTerminalStatus status) noexcept {
                    static_cast<TerminalLog*>(context)->entries.emplace_back(token, status);
                }};
    }
    std::size_t count(const SlotToken& token) const {
        return static_cast<std::size_t>(
            std::count_if(entries.begin(), entries.end(),
                          [&](const auto& entry) { return entry.first == token; }));
    }
};

SharedIoArena::Config probe_config(std::uint32_t slots, StorageKind storage = StorageKind::Staged) {
    return {.slots = slots,
            .input_bytes_per_slot = kFrames * sizeof(float),
            .output_bytes_per_slot = kFrames * sizeof(float),
            .storage_kind = storage};
}

float probe_input(std::uint64_t sequence, std::uint32_t frame) {
    return static_cast<float>(0.25 * std::sin(0.37 * frame + 0.11 * double(sequence)) -
                              0.01 * double(frame) + double(sequence));
}

std::optional<SlotToken> submit_probe(SharedIoArena& arena, std::uint64_t sequence,
                                      bool& accepted) {
    auto lease = arena.grant_write(sequence);
    if (!lease)
        return std::nullopt;
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        const float value = probe_input(sequence, frame);
        std::memcpy(lease->bytes.data() + frame * sizeof(float), &value, sizeof(float));
    }
    if (!arena.publish_written({lease->token}))
        return std::nullopt;
    accepted = arena.submit(lease->token);
    return lease->token;
}

// Claims the delivered output for `token`, checks the P1 probe, releases it.
bool probe_output_matches(SharedIoArena& arena, const SlotToken& token) {
    auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
    if (!output)
        return false;
    bool matches = output->bytes.size() == kFrames * sizeof(float);
    for (std::uint32_t frame = 0; matches && frame < kFrames; ++frame) {
        float actual = 0.0f;
        std::memcpy(&actual, output->bytes.data() + frame * sizeof(float), sizeof(float));
        const float expected = probe_input(token.stream_sequence, frame) * 1.75f + 0.25f;
        matches = std::abs(actual - expected) <= 1.0e-6f;
    }
    return arena.release_output({output->token}) && matches;
}

} // namespace

TEST_CASE("CPU shared-I/O provider reports its own authenticated identity",
          "[gpu-audio][shared-io][cpu-provider]") {
    CpuSharedIoArenaProvider provider;
    const auto identity = provider.provider_identity();
    CHECK(identity.provider == GpuAudioProvider::Cpu);
    CHECK(identity.authenticated);
    CHECK(identity.adapter_backend == "cpu");
    CHECK(identity.adapter_name == "Pulp CPU provider");
    CHECK(identity.provider_revision == "pulp-cpu-provider-1");
    // No native graphics runtime exists to observe, so the gate reports it as
    // unavailable rather than authenticated.
    CHECK_FALSE(identity.native_runtime_authenticated);
    CHECK(pulp::gpu_audio::detail::gate_shared_io_provider_identity(identity) ==
          pulp::gpu_audio::detail::SharedIoProviderIdentityGate::NativeRuntimeUnavailable);

    const auto capabilities = provider.provider_capabilities();
    CHECK_FALSE(capabilities.imported_host_pointer);
    CHECK_FALSE(capabilities.gpu_timestamps);
    CHECK_FALSE(capabilities.device_loss_recovery);
    CHECK(capabilities.completion_service);
    CHECK(capabilities.ordered_causal_state);
    CHECK_FALSE(provider.device_lost());
    std::uint64_t elapsed = 0;
    CHECK_FALSE(provider.gpu_elapsed_ns({}, elapsed));
}

TEST_CASE("CPU provider slots are host memory with a retire-then-destroy lifecycle",
          "[gpu-audio][shared-io][cpu-provider]") {
    CpuSharedIoArenaProvider provider;
    SharedIoArenaProvider::SlotResources resources;
    REQUIRE(provider.create_slot(0, 64, 32, resources));
    CHECK(resources.input != nullptr);
    CHECK(resources.output != nullptr);
    CHECK(resources.input_size == 64);
    CHECK(resources.output_size == 32);
    CHECK(resources.storage_kind == StorageKind::Staged);
    CHECK(resources.input_lifecycle.allocated);
    CHECK_FALSE(resources.input_lifecycle.import_attempted);
    CHECK_FALSE(resources.output_lifecycle.import_succeeded);

    SharedIoArenaProvider::SlotResources duplicate;
    CHECK_FALSE(provider.create_slot(0, 64, 32, duplicate));
    SharedIoArenaProvider::SlotResources unaligned;
    CHECK_FALSE(provider.create_slot(1, 6, 32, unaligned));

    SharedIoArenaProvider::SlotBufferHandle handle;
    REQUIRE(provider.acquire_slot_buffers(resources, handle));
    CHECK(provider.validate_slot_buffers(handle));

    // Destroying a live slot is refused; retirement revokes its handles.
    provider.destroy_slot(resources);
    CHECK(resources.opaque != nullptr);
    provider.retire_slot(resources);
    CHECK_FALSE(provider.validate_slot_buffers(handle));
    CHECK_FALSE(provider.acquire_slot_buffers(resources, handle));
    REQUIRE(provider.drain());
    provider.destroy_slot(resources);
    CHECK(resources.opaque == nullptr);
    CHECK(resources.input_lifecycle.host_freed);
    CHECK(resources.output_lifecycle.host_freed);
    CHECK(provider.stats().slots_created == 1);
    CHECK(provider.stats().slots_destroyed == 1);
}

TEST_CASE("CPU provider refuses an imported-host-pointer arena",
          "[gpu-audio][shared-io][cpu-provider]") {
    CpuSharedIoArenaProvider provider;
    SharedIoArena arena;
    CHECK_FALSE(arena.prepare(provider, probe_config(2, StorageKind::ImportedHostPointer)));
    CHECK(provider.stats().slots_created == provider.stats().slots_destroyed);
    CHECK(arena.prepare(provider, probe_config(2)));
    CHECK(arena.release());
}

TEST_CASE("CPU provider retires every accepted submission exactly once",
          "[gpu-audio][shared-io][cpu-provider]") {
    CpuSharedIoArenaProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, probe_config(2)));
    TerminalLog log;

    for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
        bool accepted = false;
        const auto token = submit_probe(arena, sequence, accepted);
        REQUIRE(token);
        REQUIRE(accepted);
        const auto drained = arena.drain_completions(log.observer());
        CHECK(drained.accepted == 1);
        CHECK(drained.rejected_stale_or_duplicate == 0);
        CHECK(log.count(*token) == 1);
        CHECK(probe_output_matches(arena, *token));
        // Further service publishes nothing more for that submission.
        CHECK(arena.drain_completions(log.observer()).accepted == 0);
        CHECK(log.count(*token) == 1);
    }
    CHECK(log.entries.size() == 6);
    CHECK(std::all_of(log.entries.begin(), log.entries.end(), [](const auto& entry) {
        return entry.second == SharedIoTerminalStatus::RetiredSuccess;
    }));
    CHECK(provider.stats().submissions == 6);
    CHECK(provider.stats().retired_success == 6);
    CHECK(arena.rejected_terminal_callbacks() == 0);
    CHECK(arena.release());
    CHECK(provider.stats().slots_destroyed == 2);
}

TEST_CASE("CPU provider drain publishes in-flight terminals and resumes",
          "[gpu-audio][shared-io][cpu-provider]") {
    CpuSharedIoArenaProvider provider;
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, probe_config(2)));
    TerminalLog log;

    std::vector<SlotToken> inflight;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        bool accepted = false;
        const auto token = submit_probe(arena, sequence, accepted);
        REQUIRE(token);
        REQUIRE(accepted);
        inflight.push_back(*token);
    }
    // No service has run: both terminals are still owned by the provider.
    CHECK_FALSE(provider.can_resume_after_drain());
    arena.begin_retirement();
    REQUIRE(provider.drain());
    // drain() published both before returning, so consuming the inbox without
    // further service observes them.
    const auto drained = arena.drain_completions(log.observer());
    CHECK(drained.accepted == 2);
    for (const auto& token : inflight) {
        CHECK(log.count(token) == 1);
        CHECK(probe_output_matches(arena, token));
    }
    CHECK(provider.can_resume_after_drain());
    REQUIRE(arena.reset_when_quiescent());

    bool accepted = false;
    const auto resumed = submit_probe(arena, 3, accepted);
    REQUIRE(resumed);
    CHECK(accepted);
    CHECK(resumed->preparation_epoch > inflight.front().preparation_epoch);
    arena.drain_completions(log.observer());
    CHECK(log.count(*resumed) == 1);
    CHECK(probe_output_matches(arena, *resumed));
    CHECK(arena.release());
    // A second drain after teardown is a clean, repeatable barrier.
    CHECK(provider.drain());
}

TEST_CASE("CPU provider faults keep the terminal contract",
          "[gpu-audio][shared-io][cpu-provider]") {
    SECTION("a refused submission publishes nothing") {
        CpuSharedIoArenaProvider provider(
            {.fault = CpuSharedIoArenaProvider::Fault::RejectBeforeSubmit});
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, probe_config(2)));
        TerminalLog log;
        bool accepted = true;
        const auto token = submit_probe(arena, 1, accepted);
        REQUIRE(token);
        CHECK(token->slot == 0);
        CHECK_FALSE(accepted);
        arena.drain_completions(log.observer());
        CHECK(log.entries.empty());
        CHECK(arena.available_slots() == 2);
        CHECK(provider.stats().refused_submissions == 1);
        CHECK(arena.release());
    }
    SECTION("failed execution retires failed exactly once") {
        CpuSharedIoArenaProvider provider(
            {.fault = CpuSharedIoArenaProvider::Fault::FailExecution});
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, probe_config(1)));
        TerminalLog log;
        bool accepted = false;
        const auto token = submit_probe(arena, 1, accepted);
        REQUIRE(token);
        REQUIRE(accepted);
        arena.drain_completions(log.observer());
        REQUIRE(log.count(*token) == 1);
        CHECK(log.entries.front().second == SharedIoTerminalStatus::RetiredFailed);
        CHECK_FALSE(arena.acquire_output(token->preparation_epoch, token->stream_sequence));
        CHECK(arena.discard(*token));
        CHECK(arena.release());
    }
    SECTION("a delayed terminal is withheld from service and published by drain") {
        CpuSharedIoArenaProvider provider(
            {.fault = CpuSharedIoArenaProvider::Fault::DelayCompletion});
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, probe_config(1)));
        TerminalLog log;
        bool accepted = false;
        const auto token = submit_probe(arena, 1, accepted);
        REQUIRE(token);
        REQUIRE(accepted);
        for (int pass = 0; pass < 4; ++pass)
            arena.drain_completions(log.observer());
        CHECK(log.count(*token) == 0);
        arena.begin_retirement();
        REQUIRE(provider.drain());
        arena.drain_completions(log.observer());
        CHECK(log.count(*token) == 1);
        CHECK(arena.release());
    }
    SECTION("a busy inbox cell is retained and retried once") {
        CpuSharedIoArenaProvider provider(
            {.fault = CpuSharedIoArenaProvider::Fault::HoldTerminalBusy});
        SharedIoArena arena;
        REQUIRE(arena.prepare(provider, probe_config(1)));
        TerminalLog log;
        bool accepted = false;
        const auto token = submit_probe(arena, 1, accepted);
        REQUIRE(token);
        REQUIRE(accepted);
        arena.drain_completions(log.observer());
        CHECK(log.count(*token) == 0);
        CHECK(provider.stats().terminal_busy_retries == 1);
        arena.drain_completions(log.observer());
        CHECK(log.count(*token) == 1);
        CHECK(arena.rejected_terminal_callbacks() == 0);
        CHECK(probe_output_matches(arena, *token));
        CHECK(arena.release());
    }
}

namespace {

std::vector<float> dft_spectrum(std::span<const float> time) {
    const auto size = time.size();
    std::vector<float> result(size * 2u, 0.0f);
    for (std::size_t bin = 0; bin < size; ++bin) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t frame = 0; frame < size; ++frame) {
            const double angle = -2.0 * 3.14159265358979323846 * double(bin * frame) / double(size);
            re += time[frame] * std::cos(angle);
            im += time[frame] * std::sin(angle);
        }
        result[2u * bin] = static_cast<float>(re / double(size));
        result[2u * bin + 1u] = static_cast<float>(im / double(size));
    }
    return result;
}

std::vector<double> direct_convolution(std::span<const float> input,
                                       std::span<const float> impulse) {
    std::vector<double> output(input.size() + impulse.size() - 1u, 0.0);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const auto first = frame >= input.size() ? frame - input.size() + 1u : 0u;
        const auto last = std::min(frame, impulse.size() - 1u);
        for (std::size_t tap = first; tap <= last; ++tap)
            output[frame] += static_cast<double>(input[frame - tap]) * impulse[tap];
    }
    return output;
}

struct ConvolutionRun {
    bool completed = false;
    std::size_t ready_blocks = 0;
    double max_error = 0.0;
    std::uint64_t submissions = 0;
};

constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kBlock = 32;
constexpr std::uint32_t kFft = 64;
constexpr std::uint32_t kIrLength = 8;
constexpr std::uint32_t kInputBlocks = 4;
constexpr std::uint32_t kCallbacks = 7;

std::vector<float> reference_impulse() {
    std::vector<float> impulse(kFft, 0.0f);
    impulse[0] = 0.75f;
    impulse[1] = -0.25f;
    impulse[3] = 0.125f;
    impulse[7] = -0.0625f;
    return impulse;
}

// End-to-end: callback ingress, CPU provider execution, OLA, callback egress,
// measured against a direct time-domain convolution of the reference impulse.
ConvolutionRun run_cpu_convolution(std::span<const float> provider_impulse) {
    ConvolutionRun run;
    const auto impulse = reference_impulse();
    const auto spectrum = dft_spectrum(provider_impulse);

    std::array<std::vector<float>, kChannels> input;
    std::array<std::vector<double>, kChannels> reference;
    for (std::uint32_t channel = 0; channel < kChannels; ++channel) {
        input[channel].resize(std::size_t(kInputBlocks) * kBlock);
        for (std::size_t frame = 0; frame < input[channel].size(); ++frame)
            input[channel][frame] =
                static_cast<float>(0.23 * std::sin(0.019 * frame + channel * 0.73) +
                                   0.11 * std::cos(0.083 * frame + channel * 0.31));
        input[channel][0] += channel == 0 ? 0.71f : -0.43f;
        input[channel][97u + 19u * channel] += channel == 0 ? -0.37f : 0.59f;
        reference[channel] = direct_convolution(input[channel], impulse);
    }

    auto created =
        create_cpu_shared_io_convolution_session({.session = {.pipeline = {.capacity = 8,
                                                                           .channels = kChannels,
                                                                           .block_size = kBlock,
                                                                           .fft_size = kFft,
                                                                           .ir_length = kIrLength},
                                                              .slots = 2},
                                                  .normalized_ir_spectrum = spectrum});
    if (!created.session ||
        created.reason != CpuSharedIoConvolutionSessionCreateResult::Reason::Ready)
        return run;
    auto& session = *created.session;
    if (session.provider_identity().provider != GpuAudioProvider::Cpu)
        return run;

    std::vector<float> callback_input(std::size_t(kChannels) * kBlock);
    std::vector<float> callback_output(callback_input.size());
    std::uint64_t now_ns = 1;
    for (std::uint64_t sequence = 0; sequence < kCallbacks; ++sequence) {
        std::fill(callback_input.begin(), callback_input.end(), 0.f);
        for (std::uint32_t channel = 0; channel < kChannels; ++channel)
            for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
                const auto source = sequence * kBlock + frame;
                if (source < input[channel].size())
                    callback_input[std::size_t(channel) * kBlock + frame] = input[channel][source];
            }
        const auto callback = session.begin_callback(callback_input);
        if (!callback.valid() || callback.stamp.sequence != sequence)
            return run;
        const auto delivery = session.consume_output(callback, callback_output);
        if (sequence >= SharedIoStampedBridge::kLeadBlocks) {
            if (delivery != SharedIoConvolutionSession::Delivery::Ready)
                return run;
            ++run.ready_blocks;
            const auto offset = std::size_t(sequence - SharedIoStampedBridge::kLeadBlocks) * kBlock;
            for (std::uint32_t channel = 0; channel < kChannels; ++channel)
                for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
                    const double expected = offset + frame < reference[channel].size()
                                                ? reference[channel][offset + frame]
                                                : 0.0;
                    const double actual = callback_output[std::size_t(channel) * kBlock + frame];
                    run.max_error = std::isfinite(actual)
                                        ? std::max(run.max_error, std::abs(actual - expected))
                                        : INFINITY;
                }
        }
        // CPU work runs inside service, so one pass retires this block.
        const auto serviced = session.service(now_ns++);
        run.submissions += serviced.submitted;
        if (serviced.fenced)
            return run;
    }
    run.completed = session.fence_for_offline() && session.release();
    return run;
}

} // namespace

TEST_CASE("CPU provider convolution session nulls against direct convolution",
          "[gpu-audio][shared-io][cpu-provider]") {
    SECTION("positive control: the reference impulse nulls") {
        const auto impulse = reference_impulse();
        const auto run = run_cpu_convolution(impulse);
        REQUIRE(run.completed);
        CHECK(run.submissions == kCallbacks);
        CHECK(run.ready_blocks == kCallbacks - SharedIoStampedBridge::kLeadBlocks);
        CHECK(run.max_error <= 1.0e-5);
    }
    SECTION("planted negative: a wrong impulse fails the null") {
        auto impulse = reference_impulse();
        impulse[3] = -impulse[3];
        const auto run = run_cpu_convolution(impulse);
        REQUIRE(run.completed);
        CHECK(run.ready_blocks == kCallbacks - SharedIoStampedBridge::kLeadBlocks);
        CHECK(run.max_error > 1.0e-2);
    }
}

TEST_CASE("CPU provider convolution program refuses shapes it does not implement",
          "[gpu-audio][shared-io][cpu-provider]") {
    CpuSharedIoArenaProvider provider;
    std::vector<float> spectrum(std::size_t(kFft) * 2u, 0.0f);
    const pulp::gpu_audio::detail::SharedIoConvolutionProgramSpec valid{.fft_size = kFft,
                                                                        .channels = 1,
                                                                        .logical_frames = kBlock,
                                                                        .ir_length = kIrLength,
                                                                        .normalized_ir_spectrum =
                                                                            spectrum};
    CHECK(provider.make_convolution_program(valid));
    auto non_power = valid;
    non_power.fft_size = 48;
    CHECK_FALSE(provider.make_convolution_program(non_power));
    auto wola = valid;
    wola.spectral_hop = kFft / 4;
    CHECK_FALSE(provider.make_convolution_program(wola));
    auto too_long = valid;
    too_long.ir_length = kFft;
    CHECK_FALSE(provider.make_convolution_program(too_long));
}

namespace {

struct WavenetFixture {
    std::array<std::uint32_t, 3> dilations0{1, 2, 4};
    std::array<std::uint32_t, 2> dilations1{1, 3};
    std::array<WavenetLayerSpec, 2> arrays{WavenetLayerSpec{.input_size = 1,
                                                            .condition_size = 1,
                                                            .channels = 3,
                                                            .kernel = 3,
                                                            .head_size = 2,
                                                            .gated = 1,
                                                            .head_bias = 1,
                                                            .dilations = dilations0},
                                           WavenetLayerSpec{.input_size = 3,
                                                            .condition_size = 1,
                                                            .channels = 2,
                                                            .kernel = 2,
                                                            .head_size = 1,
                                                            .gated = 0,
                                                            .head_bias = 0,
                                                            .dilations = dilations1}};
    float head_scale = 0.8f;
    std::vector<float> weights;

    WavenetFixture() {
        std::size_t count = 0;
        for (const auto& array : arrays) {
            const auto z = array.gated ? 2u * array.channels : array.channels;
            count += std::size_t(array.channels) * array.input_size;
            for (std::size_t layer = 0; layer < array.dilations.size(); ++layer)
                count += std::size_t(z) * array.channels * array.kernel + z + z +
                         std::size_t(array.channels) * array.channels + array.channels;
            count += std::size_t(array.head_size) * array.channels +
                     (array.head_bias ? array.head_size : 0u);
        }
        weights.resize(count + 1);
        std::uint32_t state = 0x2468ace1u;
        for (std::size_t index = 0; index < count; ++index) {
            state = state * 1664525u + 1013904223u;
            weights[index] = (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 0.6f;
        }
        weights.back() = head_scale;
    }

    WavenetProgramSpec spec(std::uint32_t block) const noexcept {
        return {.block_size = block,
                .head_scale = head_scale,
                .stream_instances = 1,
                .arrays = arrays,
                .weights = weights};
    }
};

// Whole-signal oracle written from the Dawn WaveNet program's kernels
// (rechannel, dilated gated layer, head, scale) with its flat weight order. It
// shares no code with the streaming CPU model the provider runs.
std::vector<double> reference_wavenet(const WavenetFixture& fixture, std::span<const float> x) {
    const auto frames = x.size();
    std::size_t w = 0;
    const auto& weights = fixture.weights;
    std::vector<std::vector<double>> source(frames, std::vector<double>(1));
    for (std::size_t t = 0; t < frames; ++t)
        source[t][0] = x[t];
    std::vector<std::vector<double>> prior_head;
    for (std::size_t a = 0; a < fixture.arrays.size(); ++a) {
        const auto& array = fixture.arrays[a];
        const std::size_t C = array.channels, K = array.kernel, H = array.head_size,
                          in = array.input_size, Z = array.gated ? 2 * C : C;
        std::vector<std::vector<double>> act(frames, std::vector<double>(C, 0.0));
        for (std::size_t t = 0; t < frames; ++t)
            for (std::size_t oc = 0; oc < C; ++oc)
                for (std::size_t ic = 0; ic < in; ++ic)
                    act[t][oc] += weights[w + oc * in + ic] * source[t][ic];
        w += C * in;
        std::vector<std::vector<double>> headacc =
            a == 0 ? std::vector<std::vector<double>>(frames, std::vector<double>(C, 0.0))
                   : prior_head;
        for (const auto dilation : array.dilations) {
            const auto conv_w = w;
            const auto conv_b = conv_w + Z * C * K;
            const auto mix_w = conv_b + Z;
            const auto l1_w = mix_w + Z;
            const auto l1_b = l1_w + C * C;
            w = l1_b + C;
            std::vector<std::vector<double>> next(frames, std::vector<double>(C, 0.0));
            for (std::size_t t = 0; t < frames; ++t) {
                std::vector<double> z(Z), activation(C);
                for (std::size_t oc = 0; oc < Z; ++oc) {
                    double acc = weights[conv_b + oc];
                    for (std::size_t k = 0; k < K; ++k) {
                        const auto back = std::size_t(dilation) * (K - 1 - k);
                        if (t < back)
                            continue;
                        for (std::size_t ic = 0; ic < C; ++ic)
                            acc += weights[conv_w + oc * C * K + ic * K + k] * act[t - back][ic];
                    }
                    z[oc] = acc + weights[mix_w + oc] * x[t];
                }
                for (std::size_t c = 0; c < C; ++c) {
                    const double gate = array.gated ? 1.0 / (1.0 + std::exp(-z[C + c])) : 1.0;
                    activation[c] = std::tanh(z[c]) * gate;
                    headacc[t][c] += activation[c];
                }
                for (std::size_t oc = 0; oc < C; ++oc) {
                    double r = weights[l1_b + oc];
                    for (std::size_t ic = 0; ic < C; ++ic)
                        r += weights[l1_w + oc * C + ic] * activation[ic];
                    next[t][oc] = act[t][oc] + r;
                }
            }
            act = std::move(next);
        }
        const auto head_w = w;
        const auto head_b = head_w + H * C;
        w = head_b + (array.head_bias ? H : 0);
        std::vector<std::vector<double>> head(frames, std::vector<double>(H, 0.0));
        for (std::size_t t = 0; t < frames; ++t)
            for (std::size_t h = 0; h < H; ++h) {
                double value = array.head_bias ? weights[head_b + h] : 0.0;
                for (std::size_t c = 0; c < C; ++c)
                    value += weights[head_w + h * C + c] * headacc[t][c];
                head[t][h] = value;
            }
        source = act;
        prior_head = std::move(head);
    }
    std::vector<double> y(frames);
    for (std::size_t t = 0; t < frames; ++t)
        y[t] = prior_head[t][0] * weights[w];
    return y;
}

float wavenet_input(std::size_t frame) {
    return static_cast<float>(0.4 * std::sin(0.071 * double(frame)) +
                              0.2 * std::sin(0.53 * double(frame) + 0.3));
}

// Streams `blocks` contiguous blocks through a CPU WaveNet program session.
std::optional<std::vector<float>> run_cpu_wavenet(const WavenetProgramSpec& spec,
                                                  std::uint32_t blocks) {
    auto provider = std::make_unique<CpuSharedIoArenaProvider>();
    auto program = provider->make_wavenet_program(spec);
    if (!program)
        return std::nullopt;
    SharedIoProgramSession session;
    const auto bytes = std::size_t(spec.block_size) * sizeof(float);
    if (!session.prepare({std::move(provider), std::move(program)},
                         {.slots = 2,
                          .input_bytes_per_slot = bytes,
                          .output_bytes_per_slot = bytes,
                          .storage_kind = StorageKind::Staged}))
        return std::nullopt;
    std::vector<float> output;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        auto lease = session.acquire_input(block, 0);
        if (!lease || lease->bytes.size() != bytes)
            return std::nullopt;
        for (std::uint32_t frame = 0; frame < spec.block_size; ++frame) {
            const float sample = wavenet_input(std::size_t(block) * spec.block_size + frame);
            std::memcpy(lease->bytes.data() + frame * sizeof(float), &sample, sizeof(float));
        }
        if (!session.submit(SharedIoProgramSession::SubmitToken::admit(lease->token)))
            return std::nullopt;
        session.service(block + 1u);
        const auto completion = session.pop_completion();
        if (!completion || completion->status != SharedIoArena::CompletionStatus::RetiredSuccess)
            return std::nullopt;
        auto out = session.acquire_output(*completion);
        if (!out)
            return std::nullopt;
        for (std::uint32_t frame = 0; frame < spec.block_size; ++frame) {
            float value = 0.0f;
            std::memcpy(&value, out->bytes.data() + frame * sizeof(float), sizeof(float));
            output.push_back(value);
        }
        if (!session.release_output({out->token}))
            return std::nullopt;
    }
    if (!session.release())
        return std::nullopt;
    return output;
}

} // namespace

TEST_CASE("CPU provider WaveNet program matches the Dawn program's math across blocks",
          "[gpu-audio][shared-io][cpu-provider]") {
    constexpr std::uint32_t block = 16;
    constexpr std::uint32_t blocks = 10;
    WavenetFixture fixture;
    std::vector<float> input(std::size_t(block) * blocks);
    for (std::size_t frame = 0; frame < input.size(); ++frame)
        input[frame] = wavenet_input(frame);
    const auto expected = reference_wavenet(fixture, input);

    SECTION("positive control: provider output nulls against the oracle") {
        const auto actual = run_cpu_wavenet(fixture.spec(block), blocks);
        REQUIRE(actual);
        REQUIRE(actual->size() == expected.size());
        double worst = 0.0;
        for (std::size_t index = 0; index < expected.size(); ++index)
            worst = std::max(worst, std::abs((*actual)[index] - expected[index]));
        CHECK(worst <= 1.0e-5);
        // The output is not trivially silent, so the null is meaningful.
        CHECK(std::any_of(expected.begin(), expected.end(),
                          [](double value) { return std::abs(value) > 1.0e-2; }));
    }
    SECTION("planted negative: one perturbed weight fails the null") {
        auto mutated = fixture;
        // The final head weight, directly ahead of the trailing head scale.
        mutated.weights[mutated.weights.size() - 2] += 0.25f;
        const auto actual = run_cpu_wavenet(mutated.spec(block), blocks);
        REQUIRE(actual);
        double worst = 0.0;
        for (std::size_t index = 0; index < expected.size(); ++index)
            worst = std::max(worst, std::abs((*actual)[index] - expected[index]));
        CHECK(worst > 1.0e-3);
    }
}

TEST_CASE("CPU provider WaveNet program reproduces a hand-computed causal model",
          "[gpu-audio][shared-io][cpu-provider]") {
    // The Dawn WaveNet qualification fixture: y[t] = tanh(0.5 * x[t - 1]).
    std::array<std::uint32_t, 1> dilations{1};
    const WavenetLayerSpec layer{.input_size = 1,
                                 .condition_size = 1,
                                 .channels = 1,
                                 .kernel = 2,
                                 .head_size = 1,
                                 .gated = 0,
                                 .head_bias = 0,
                                 .dilations = dilations};
    const std::array<float, 9> weights{1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};
    const WavenetProgramSpec spec{.block_size = 2,
                                  .head_scale = 1.0f,
                                  .stream_instances = 1,
                                  .arrays = {&layer, 1},
                                  .weights = weights};
    const auto actual = run_cpu_wavenet(spec, 4);
    REQUIRE(actual);
    for (std::size_t frame = 0; frame < actual->size(); ++frame) {
        const double previous = frame == 0 ? 0.0 : wavenet_input(frame - 1);
        CHECK(std::abs((*actual)[frame] - std::tanh(0.5 * previous)) <= 1.0e-6);
    }
}

TEST_CASE("CPU provider WaveNet program enforces contiguous stream order",
          "[gpu-audio][shared-io][cpu-provider]") {
    WavenetFixture fixture;
    CpuSharedIoArenaProvider provider;
    auto multi = fixture.spec(8);
    multi.stream_instances = 2;
    CHECK_FALSE(provider.make_wavenet_program(multi));

    auto owned = std::make_unique<CpuSharedIoArenaProvider>();
    auto program = owned->make_wavenet_program(fixture.spec(8));
    REQUIRE(program);
    SharedIoProgramSession session;
    REQUIRE(session.prepare({std::move(owned), std::move(program)},
                            {.slots = 2,
                             .input_bytes_per_slot = 8 * sizeof(float),
                             .output_bytes_per_slot = 8 * sizeof(float),
                             .storage_kind = StorageKind::Staged}));
    auto first = session.acquire_input(0, 0);
    REQUIRE(first);
    REQUIRE(session.submit(SharedIoProgramSession::SubmitToken::admit(first->token)));
    // Skipping sequence 1 would desynchronize resident causal history.
    auto gap = session.acquire_input(2, 0);
    REQUIRE(gap);
    const auto gap_token = SharedIoProgramSession::SubmitToken::admit(gap->token);
    CHECK_FALSE(session.submit(gap_token));
    // The refused slot was returned by the plan; nothing is left to cancel.
    CHECK_FALSE(session.cancel(gap_token));
    session.service(1);
    const auto completion = session.pop_completion();
    REQUIRE(completion);
    CHECK(completion->status == SharedIoArena::CompletionStatus::RetiredSuccess);
    CHECK(session.discard_completion(*completion));
    CHECK(session.release());
}

namespace {

std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct LossRun {
    bool accepted = false;
    std::size_t terminals_before_drain = 0;
    std::size_t terminals_after_drain = 0;
    std::optional<SharedIoTerminalStatus> status;
    bool lost = false;
    bool resumable = true;
    bool loss_recorded_in_window = false;
    bool refused_after_loss = false;
    bool new_preparation_refused = false;
    bool released = false;
};

// One submission on a provider armed with `fault`, serviced, then drained:
// the shape of the conformance kit's force-loss scenarios.
LossRun run_loss(CpuSharedIoArenaProvider::Fault fault) {
    LossRun run;
    CpuSharedIoArenaProvider provider({.fault = fault});
    SharedIoArena arena;
    if (!arena.prepare(provider, probe_config(2)))
        return run;
    TerminalLog log;
    const auto before = steady_ns();
    const auto token = submit_probe(arena, 1, run.accepted);
    if (!token)
        return run;
    for (int pass = 0; pass < 4; ++pass)
        arena.drain_completions(log.observer());
    run.terminals_before_drain = log.count(*token);
    // A second block after the loss must be refused before any work runs.
    bool second_accepted = true;
    const auto second = submit_probe(arena, 2, second_accepted);
    run.refused_after_loss = second && !second_accepted;
    arena.begin_retirement();
    if (!provider.drain())
        return run;
    arena.drain_completions(log.observer());
    run.terminals_after_drain = log.count(*token);
    for (const auto& entry : log.entries)
        if (entry.first == *token)
            run.status = entry.second;
    if (run.accepted)
        (void)arena.discard(*token);
    run.lost = provider.device_lost();
    run.resumable = provider.can_resume_after_drain();
    const auto stats = provider.stats();
    run.loss_recorded_in_window = stats.device_lost_reason != 0 &&
                                  stats.device_lost_callback_ns >= before &&
                                  stats.device_lost_callback_ns <= steady_ns();
    run.released = arena.release();
    SharedIoArena retry;
    run.new_preparation_refused = !retry.prepare(provider, probe_config(1));
    run.resumable = run.resumable || provider.can_resume_after_drain();
    return run;
}

void check_lost(const LossRun& run) {
    CHECK(run.terminals_before_drain == 0);
    CHECK(run.lost);
    CHECK_FALSE(run.resumable);
    CHECK(run.loss_recorded_in_window);
    CHECK(run.refused_after_loss);
    CHECK(run.released);
    CHECK(run.new_preparation_refused);
}

} // namespace

TEST_CASE("CPU provider without a loss fault is never lost",
          "[gpu-audio][shared-io][cpu-provider][loss]") {
    const auto run = run_loss(CpuSharedIoArenaProvider::Fault::None);
    CHECK(run.accepted);
    CHECK(run.terminals_before_drain == 1);
    CHECK(run.status == SharedIoTerminalStatus::RetiredSuccess);
    CHECK_FALSE(run.lost);
    CHECK(run.resumable);
    CHECK_FALSE(run.loss_recorded_in_window);
    CHECK_FALSE(run.refused_after_loss);
    CHECK(run.released);
    CHECK_FALSE(run.new_preparation_refused);
}

TEST_CASE("CPU provider loss before submit refuses the work and stays lost",
          "[gpu-audio][shared-io][cpu-provider][loss]") {
    const auto run = run_loss(CpuSharedIoArenaProvider::Fault::ForceLossBeforeSubmit);
    CHECK_FALSE(run.accepted);
    CHECK(run.terminals_after_drain == 0);
    check_lost(run);
}

TEST_CASE("CPU provider loss between submit and registration fails the work at drain",
          "[gpu-audio][shared-io][cpu-provider][loss]") {
    const auto run =
        run_loss(CpuSharedIoArenaProvider::Fault::ForceLossBetweenSubmitAndCompletionRegistration);
    CHECK(run.accepted);
    CHECK(run.terminals_after_drain == 1);
    CHECK(run.status == SharedIoTerminalStatus::RetiredFailed);
    check_lost(run);
}

TEST_CASE("CPU provider loss after registration fails the work at drain",
          "[gpu-audio][shared-io][cpu-provider][loss]") {
    const auto run =
        run_loss(CpuSharedIoArenaProvider::Fault::ForceLossAfterCompletionRegistration);
    CHECK(run.accepted);
    CHECK(run.terminals_after_drain == 1);
    CHECK(run.status == SharedIoTerminalStatus::RetiredFailed);
    check_lost(run);
}

TEST_CASE("CPU provider drain after loss retires every in-flight block failed",
          "[gpu-audio][shared-io][cpu-provider][loss]") {
    // The loss strikes once the first block's terminal is registered; that
    // block stays in flight, unpublished by service, until drain.
    CpuSharedIoArenaProvider provider(
        {.fault = CpuSharedIoArenaProvider::Fault::ForceLossAfterCompletionRegistration});
    SharedIoArena arena;
    REQUIRE(arena.prepare(provider, probe_config(2)));
    TerminalLog log;
    bool accepted = false;
    const auto first = submit_probe(arena, 1, accepted);
    REQUIRE(first);
    REQUIRE(accepted);
    CHECK(provider.device_lost());
    for (int pass = 0; pass < 4; ++pass)
        arena.drain_completions(log.observer());
    CHECK(log.entries.empty());
    arena.begin_retirement();
    REQUIRE(provider.drain());
    arena.drain_completions(log.observer());
    REQUIRE(log.count(*first) == 1);
    CHECK(log.entries.front().second == SharedIoTerminalStatus::RetiredFailed);
    CHECK(arena.discard(*first));
    CHECK_FALSE(provider.can_resume_after_drain());
    CHECK(arena.release());
    CHECK(provider.device_lost());
    CHECK_FALSE(provider.can_resume_after_drain());
    CHECK(provider.stats().retired_success == 0);
}
