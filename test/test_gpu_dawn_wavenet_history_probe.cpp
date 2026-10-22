#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_wavenet_spec.hpp"
#include "support/dawn_transfer_call_counter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {

using namespace pulp::gpu_audio::detail;

struct Fixture {
    // A deliberately mixed dilation pair. With kernel=2 the two causal pads are
    // one and four samples; with block_size=2 their column rings hold 4 and 8
    // columns (16 and 32 bytes), and the final ring 2 columns (8 bytes).
    std::array<std::uint32_t, 2> dilations{1, 4};
    WavenetLayerSpec layer{
        .input_size = 1,
        .condition_size = 1,
        .channels = 1,
        .kernel = 2,
        .head_size = 1,
        .gated = 0,
        .head_bias = 0,
        .dilations = dilations,
    };
    // rechannel (1) + two causal layers (6 each: conv k0, conv k1, bias, mixin,
    // 1x1, 1x1 bias) + head (1) + scale (1). Non-zero so the streamed output
    // depends on every tap of both dilations.
    std::array<float, 15> weights{0.9f, 0.6f, -0.4f,  0.05f, 0.3f,  0.7f, -0.02f, -0.5f,
                                  0.8f, 0.1f, -0.25f, 0.4f,  0.03f, 1.1f, 1.0f};

    WavenetProgramSpec spec() const noexcept {
        return {.block_size = 2,
                .head_scale = 1.0f,
                .stream_instances = 1,
                .arrays = {&layer, 1},
                .weights = weights};
    }
};

// Independent double-precision reference for the fixture over a whole stream,
// with zero state before the first sample.
std::vector<double> reference(const Fixture& f, const std::vector<float>& x) {
    const auto& w = f.weights;
    const std::size_t T = x.size();
    std::vector<double> act(T), head(T, 0.0), out(T);
    for (std::size_t t = 0; t < T; ++t)
        act[t] = static_cast<double>(w[0]) * x[t];
    std::size_t off = 1;
    for (const auto dilation : f.dilations) {
        std::vector<double> next(T);
        for (std::size_t t = 0; t < T; ++t) {
            const double back = t >= dilation ? act[t - dilation] : 0.0;
            const double z = static_cast<double>(w[off + 2]) + w[off] * back + w[off + 1] * act[t] +
                             static_cast<double>(w[off + 3]) * x[t];
            const double a = std::tanh(z);
            head[t] += a;
            next[t] = act[t] + w[off + 5] + w[off + 4] * a;
        }
        act.swap(next);
        off += 6;
    }
    for (std::size_t t = 0; t < T; ++t)
        out[t] = static_cast<double>(w[off]) * head[t] * w.back();
    return out;
}

bool wait_for_completion(SharedIoComputePlan& plan,
                         std::optional<SharedIoComputePlan::Completion>& completion) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completion && std::chrono::steady_clock::now() < deadline) {
        plan.drain(std::numeric_limits<std::uint64_t>::max());
        completion = plan.pop_completion();
        if (!completion)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return completion.has_value();
}

} // namespace

int main() {
    const auto fail = [](std::string_view stage) {
        std::cerr << "mixed_dilation_history_proof status=failed stage=" << stage << '\n';
        return 1;
    };
    Fixture fixture;
    if (!validate_wavenet_spec(fixture.spec()).accepted())
        return fail("fixture_validation");

    pulp::test::DawnTransferCallCounter transfer_counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = transfer_counter.deferred_proc_table(),
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider)
        return 77;
    auto& provider = *created.provider;

    auto program = DawnSharedIoWavenetProgram::create(provider, fixture.spec());
    if (!program)
        return fail("program_create");

    SharedIoComputePlan plan;
    if (!plan.prepare(provider,
                      {.slots = 2,
                       .input_bytes_per_slot = 2u * sizeof(float),
                       .output_bytes_per_slot = 2u * sizeof(float),
                       .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer},
                      std::move(program)))
        return fail("plan_prepare");

    // Preparation zeroes one ring per activation, sized from that layer's own
    // causal pad. Each target-size bucket is fully accounted for:
    //   8 bytes:  final activation ring + device stream position
    //   16 bytes: dilation-1 ring + scale uniform
    //   32 bytes: dilation-4 ring + rechannel uniform + head uniform
    //   64 bytes: the two layer uniforms
    const auto prepared = transfer_counter.snapshot();
    const bool exact_allocations = prepared.queue_write_buffer_target_size_histogram[8] == 2 &&
                                   prepared.queue_write_buffer_target_size_histogram[16] == 2 &&
                                   prepared.queue_write_buffer_target_size_histogram[32] == 3 &&
                                   prepared.queue_write_buffer_target_size_histogram[64] == 2 &&
                                   prepared.queue_write_buffer_target_size_histogram[4] == 0 &&
                                   prepared.queue_write_buffer_target_size_histogram[12] == 0;
    if (!exact_allocations) {
        std::cerr << "mixed_dilation_history_proof status=failed stage=allocations"
                  << " queue_writes=" << prepared.queue_write_buffer_calls
                  << " bytes=" << prepared.queue_write_buffer_bytes
                  << " target8=" << prepared.queue_write_buffer_target_size_histogram[8]
                  << " target16=" << prepared.queue_write_buffer_target_size_histogram[16]
                  << " target32=" << prepared.queue_write_buffer_target_size_histogram[32]
                  << " target64=" << prepared.queue_write_buffer_target_size_histogram[64] << '\n';
        return 1;
    }

    // Stream 12 two-sample blocks through two slots: the dilation-4 taps reach
    // two blocks back and every ring wraps several times. Each block must match
    // the reference computed over the whole stream.
    transfer_counter.reset();
    constexpr std::uint64_t blocks = 12;
    std::vector<float> stream(blocks * 2u), streamed(blocks * 2u, -9.0f);
    for (std::size_t i = 0; i < stream.size(); ++i)
        stream[i] = 0.7f * std::sin(0.9f * static_cast<float>(i)) - 0.1f;
    for (std::uint64_t sequence = 1; sequence <= blocks; ++sequence) {
        auto write = plan.acquire_input(sequence, 0);
        if (!write || write->bytes.size() != 2u * sizeof(float))
            return fail("input_lease");
        auto* input = reinterpret_cast<float*>(write->bytes.data());
        input[0] = stream[(sequence - 1u) * 2u];
        input[1] = stream[(sequence - 1u) * 2u + 1u];
        if (!plan.submit({write->token, 0}))
            return fail("submit");
        std::optional<SharedIoComputePlan::Completion> completion;
        if (!wait_for_completion(plan, completion))
            return fail("completion");
        auto output = plan.acquire_output(*completion);
        if (!output || output->bytes.size() != 2u * sizeof(float))
            return fail("output");
        const auto* result = reinterpret_cast<const float*>(output->bytes.data());
        streamed[(sequence - 1u) * 2u] = result[0];
        streamed[(sequence - 1u) * 2u + 1u] = result[1];
        if (!plan.release_output({output->token}))
            return fail("output_release");
    }
    const auto expected = reference(fixture, stream);
    double worst = 0.0;
    for (std::size_t i = 0; i < stream.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(streamed[i]) - expected[i]));
    if (!(worst < 1e-5)) {
        std::cerr << "mixed_dilation_history_proof status=failed stage=stream_parity worst="
                  << worst << '\n';
        return 1;
    }

    // Submissions read and extend the rings in place and advance the stream
    // position on the device: no buffer copies and no host writes.
    const auto submitted = transfer_counter.snapshot();
    const bool zero_transfers = submitted.copy_buffer_to_buffer_calls == 0 &&
                                submitted.copy_buffer_to_buffer_bytes == 0 &&
                                submitted.queue_write_buffer_calls == 0;
    const bool released = plan.release() && provider.drain();
    if (!zero_transfers || !released) {
        std::cerr << "mixed_dilation_history_proof status=failed stage=transfers"
                  << " copies=" << submitted.copy_buffer_to_buffer_calls
                  << " copy_bytes=" << submitted.copy_buffer_to_buffer_bytes
                  << " writes=" << submitted.queue_write_buffer_calls << " released=" << released
                  << '\n';
        return 1;
    }

    std::cout << "mixed_dilation_history_proof"
              << " ring_bytes=[16,32,8]"
              << " copies=0 blocks=" << blocks << " worst=" << worst << " status=passed\n";
    return 0;
}
