#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_wavenet_spec.hpp"
#include "support/dawn_transfer_call_counter.hpp"

#include <array>
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
    // A deliberately mixed dilation pair. With kernel=2 the two causal pads
    // are one and two samples, so history transfers must be 4 and 8 bytes.
    std::array<std::uint32_t, 2> dilations{1, 2};
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
    // rechannel (1) + two causal layers (6 each) + head (1) + scale (1).
    std::array<float, 15> weights{};

    Fixture() {
        weights.back() = 1.0f;
    }

    WavenetProgramSpec spec() const noexcept {
        return {.block_size = 2,
                .head_scale = 1.0f,
                .stream_instances = 1,
                .arrays = {&layer, 1},
                .weights = weights};
    }
};

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
                      {.slots = 1,
                       .input_bytes_per_slot = 2u * sizeof(float),
                       .output_bytes_per_slot = 2u * sizeof(float),
                       .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer},
                      std::move(program)))
        return fail("plan_prepare");

    // Preparation writes one zeroed history buffer per non-final activation.
    // These exact frontend writes bind the two provider allocations to the
    // independently calculated 4-byte and 8-byte causal pads.
    const auto prepared = transfer_counter.snapshot();
    const bool exact_allocations =
        prepared.queue_write_buffer_target_size_histogram[4] == 1 &&
        prepared.queue_write_buffer_target_size_histogram[8] == 2 &&
        prepared.queue_write_buffer_target_size_histogram[12] == 1 &&
        prepared.queue_write_buffer_target_size_histogram[16] == 2;
    if (!exact_allocations) {
        std::cerr << "mixed_dilation_history_proof status=failed stage=allocations"
                  << " queue_writes=" << prepared.queue_write_buffer_calls
                  << " bytes=" << prepared.queue_write_buffer_bytes
                  << " target4=" << prepared.queue_write_buffer_target_size_histogram[4]
                  << " target8=" << prepared.queue_write_buffer_target_size_histogram[8]
                  << " target12=" << prepared.queue_write_buffer_target_size_histogram[12]
                  << " target16=" << prepared.queue_write_buffer_target_size_histogram[16]
                  << " write4=" << prepared.queue_write_buffer_size_histogram[4]
                  << " write8=" << prepared.queue_write_buffer_size_histogram[8] << '\n';
        return 1;
    }

    transfer_counter.reset();
    auto write = plan.acquire_input(1, 0);
    if (!write || write->bytes.size() != 2u * sizeof(float))
        return fail("input_lease");
    auto* input = reinterpret_cast<float*>(write->bytes.data());
    input[0] = 0.25f;
    input[1] = 0.5f;
    if (!plan.submit({write->token, 0}))
        return fail("submit");

    std::optional<SharedIoComputePlan::Completion> completion;
    if (!wait_for_completion(plan, completion))
        return fail("completion");
    auto output = plan.acquire_output(*completion);
    if (!output || !plan.release_output({output->token}))
        return fail("output");

    // One submission copies each history into the activation prefix and then
    // copies each layer tail back into its resident history. The mixed pads
    // therefore produce two 4-byte and two 8-byte copies exactly.
    const auto submitted = transfer_counter.snapshot();
    const bool exact_copies =
        submitted.copy_buffer_to_buffer_calls == 4 &&
        submitted.copy_buffer_to_buffer_bytes == 24 &&
        submitted.copy_buffer_to_buffer_size_histogram[4] == 2 &&
        submitted.copy_buffer_to_buffer_size_histogram[8] == 2;
    const bool released = plan.release() && provider.drain();
    if (!exact_copies || !released) {
        std::cerr << "mixed_dilation_history_proof status=failed stage=copies"
                  << " calls=" << submitted.copy_buffer_to_buffer_calls
                  << " bytes=" << submitted.copy_buffer_to_buffer_bytes
                  << " size4=" << submitted.copy_buffer_to_buffer_size_histogram[4]
                  << " size8=" << submitted.copy_buffer_to_buffer_size_histogram[8]
                  << " released=" << released << '\n';
        return 1;
    }

    std::cout << "mixed_dilation_history_proof"
              << " allocations_bytes=[4,8]"
              << " copy_bytes=[4,4,8,8]"
              << " status=passed\n";
    return 0;
}
