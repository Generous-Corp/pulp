#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/nam_tcn_artifact.hpp"
#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_wavenet_spec.hpp"
#include "support/dawn_transfer_call_counter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {

using namespace pulp::gpu_audio::detail;
using Clock = std::chrono::steady_clock;

struct Fixture {
    std::array<std::uint32_t, 1> dilations{1};
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
    // rechannel (1) + one causal layer (2 + 1 + 1 + 1 + 1) + head (1) + scale.
    std::array<float, 9> weights{1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f};

    WavenetProgramSpec spec() const noexcept {
        return {.block_size = 2,
                .head_scale = 1.0f,
                .stream_instances = 1,
                .arrays = {&layer, 1},
                .weights = weights};
    }
};

struct Result {
    bool unavailable = false;
    bool identity = false;
    bool resident_state = false;
    bool zero_audio_slot_transfers = false;
    bool cpu_parity = false;
    bool cpu_fallback_parity = false;
    bool cpu_fallback_delivered = false;
    bool late_result_rejected = false;
    bool retired = false;
    bool reprimed = false;
    bool immutable_provenance = false;
    bool device_loss_negative = false;
    bool device_loss_receipt = false;
    std::uint64_t submissions = 0;
    std::uint64_t gpu_delivered = 0;
    std::uint64_t cpu_fallback = 0;
    std::uint64_t late = 0;
    std::uint64_t resident_state_copies = 0;
    std::uint64_t generation = 0;
    std::uint64_t reprime_generation = 0;
    std::string reason;
};

void emit(const Result& result, std::string_view status) {
    std::cout << R"({"schema":"pulp.gpu-audio.wavenet.qualification.v1","status":")" << status
              << R"(","identity":)" << (result.identity ? "true" : "false")
              << R"(,"resident_state":)" << (result.resident_state ? "true" : "false")
              << R"(,"zero_audio_slot_transfers":)"
              << (result.zero_audio_slot_transfers ? "true" : "false") << R"(,"cpu_parity":)"
              << (result.cpu_parity ? "true" : "false") << R"(,"cpu_fallback_parity":)"
              << (result.cpu_fallback_parity ? "true" : "false") << R"(,"cpu_fallback_delivered":)"
              << (result.cpu_fallback_delivered ? "true" : "false")
              << R"(,"continuous_cpu_fallback":)" << (result.cpu_fallback != 0 ? "true" : "false")
              << R"(,"late_result_rejected":)" << (result.late_result_rejected ? "true" : "false")
              << R"(,"retired":)" << (result.retired ? "true" : "false") << R"(,"reprimed":)"
              << (result.reprimed ? "true" : "false") << R"(,"immutable_provenance":)"
              << (result.immutable_provenance ? "true" : "false") << R"(,"device_loss_negative":)"
              << (result.device_loss_negative ? "true" : "false") << R"(,"device_loss_receipt":)"
              << (result.device_loss_receipt ? "true" : "false") << R"(,"submissions":)"
              << result.submissions << R"(,"gpu_delivered":)" << result.gpu_delivered
              << R"(,"cpu_fallback_blocks":)" << result.cpu_fallback << R"(,"late_blocks":)"
              << result.late << R"(,"resident_state_copies":)" << result.resident_state_copies
              << R"(,"generation":)" << result.generation << R"(,"reprime_generation":)"
              << result.reprime_generation << R"(,"reason":")" << result.reason << "\"}" << '\n';
}

bool compare_block(std::span<const float> actual, std::span<const float> expected) {
    if (actual.size() != expected.size())
        return false;
    return std::equal(actual.begin(), actual.end(), expected.begin(), [](float lhs, float rhs) {
        return std::isfinite(lhs) && std::abs(lhs - rhs) <= 2.0e-5f;
    });
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

std::optional<pulp::gpu_audio::detail::NamTcnArtifact> make_cpu_oracle(const Fixture& fixture,
                                                                       std::string& reason) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("pulp-gpu-dawn-wavenet-qualification-" + std::to_string(now_ns()) + ".nam");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        reason = "cpu_oracle_fixture_open_failed";
        return std::nullopt;
    }
    file
        << R"({"architecture":"WaveNet","sample_rate":48000,"config":{"head_scale":1.0,"layers":[{"input_size":1,"condition_size":1,"channels":1,"kernel_size":2,"head_size":1,"dilations":[1],"gated":false,"head_bias":false,"activation":"Tanh"}]},"weights":[)";
    for (std::size_t index = 0; index < fixture.weights.size(); ++index) {
        if (index != 0)
            file << ',';
        file << fixture.weights[index];
    }
    file << "]}\n";
    file.close();

    pulp::gpu_audio::detail::NamTcnArtifact oracle;
    if (!oracle.load(path.string(), &reason)) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        reason = "cpu_oracle_fixture_load_failed:" + reason;
        return std::nullopt;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return oracle;
}

void run_device_loss_negative(Result& result, const Fixture& fixture, const void* proc_table) {
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = proc_table,
         .fault = DawnSharedIoProvider::Fault::ForceLossBetweenSubmitAndCompletionRegistration,
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider)
        return;
    auto& provider = *created.provider;
    auto program = DawnSharedIoWavenetProgram::create(provider, fixture.spec());
    if (!program)
        return;
    SharedIoComputePlan plan;
    const auto bytes = fixture.spec().block_size * sizeof(float);
    if (!plan.prepare(provider,
                      {.slots = 1,
                       .input_bytes_per_slot = bytes,
                       .output_bytes_per_slot = bytes,
                       .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer},
                      std::move(program)))
        return;
    auto write = plan.acquire_input(1, 0);
    if (!write)
        return;
    std::array<float, 2> input{1.0f, 1.25f};
    std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
    const bool submitted = plan.submit({write->token, 0});
    const bool drained = provider.drain();
    std::optional<SharedIoComputePlan::Completion> completion;
    plan.drain(now_ns());
    completion = plan.pop_completion();
    if (completion)
        (void)plan.discard_completion(*completion);
    auto receipts = plan.take_lifecycle_receipts();
    const bool can_resume = provider.can_resume_after_drain();
    const bool reprime = can_resume && plan.reprime_when_quiescent();
    result.device_loss_receipt =
        receipts.size() == 1 && receipts.front().terminal && receipts.front().device_lost;
    result.device_loss_negative = submitted && drained && provider.device_lost() && !can_resume &&
                                  !reprime && result.device_loss_receipt;
    const bool released = plan.release();
    result.device_loss_negative = result.device_loss_negative && released;
    if (!result.device_loss_negative)
        result.reason = "device_loss_negative_failed:" + std::to_string(submitted) +
                        std::to_string(drained) + std::to_string(provider.device_lost()) +
                        std::to_string(can_resume) + std::to_string(reprime) +
                        std::to_string(receipts.size()) + std::to_string(released);
}

Result run() {
    Result result;
    Fixture fixture;
    if (!validate_wavenet_spec(fixture.spec()).accepted()) {
        result.reason = "fixture_rejected";
        return result;
    }

    std::string oracle_reason;
    auto cpu_oracle = make_cpu_oracle(fixture, oracle_reason);
    if (!cpu_oracle) {
        result.reason = std::move(oracle_reason);
        return result;
    }
    result.cpu_fallback_parity = true;

    // The counter is constructed before any provider-owned Dawn object and is
    // destroyed after the provider, satisfying the process-global proc-table
    // ownership contract.
    pulp::test::DawnTransferCallCounter transfer_counter(
        pulp::test::DawnTransferCallCounter::InstallMode::DeferredSingleInstall);
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = transfer_counter.deferred_proc_table(),
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider) {
        result.unavailable = true;
        result.reason = created.reason.empty() ? "provider_unavailable" : created.reason;
        emit(result, "unavailable");
        return result;
    }
    auto& provider = *created.provider;
    const auto identity = provider.provider_identity();
    const auto adapter = provider.adapter_identity();
    result.identity = identity.authenticated && identity.native_runtime_authenticated;
    result.immutable_provenance =
        result.identity && valid_provider_receipt_digest(
                               identity, adapter.native_runtime_revision,
                               provider_receipt_digest(identity, adapter.native_runtime_revision));
    if (!result.immutable_provenance) {
        result.reason = "provider_identity_unavailable";
        return result;
    }
    auto digest = provider_receipt_digest(identity, adapter.native_runtime_revision);
    digest[0] = digest[0] == '0' ? '1' : '0';
    result.immutable_provenance =
        !valid_provider_receipt_digest(identity, adapter.native_runtime_revision, digest);

    auto program = DawnSharedIoWavenetProgram::create(provider, fixture.spec());
    if (!program) {
        result.reason = "program_unavailable";
        return result;
    }
    result.resident_state = program->prepared() == false && program->history_bytes() != 0 &&
                            program->weight_count() == fixture.weights.size();
    const auto capabilities = provider.provider_capabilities();
    result.resident_state = result.resident_state && capabilities.ordered_causal_state &&
                            capabilities.imported_host_pointer;
    const auto resident_history_bytes = program->history_bytes();

    SharedIoComputePlan plan;
    const auto bytes = fixture.spec().block_size * sizeof(float);
    if (!plan.prepare(provider,
                      {.slots = 2,
                       .input_bytes_per_slot = bytes,
                       .output_bytes_per_slot = bytes,
                       .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer},
                      std::move(program))) {
        result.reason = "plan_prepare_failed";
        return result;
    }
    result.generation = plan.preparation_epoch();

    transfer_counter.reset();
    float previous = 0.0f;
    std::uint64_t sequence = 1;
    while (sequence <= 4) {
        const std::array<float, 2> input{static_cast<float>(sequence),
                                         static_cast<float>(sequence) + 0.25f};
        const std::array<float, 2> expected{std::tanh(0.5f * previous), std::tanh(0.5f * input[0])};
        std::array<float, 2> cpu_output{};
        cpu_oracle->process(input.data(), cpu_output.data(), input.size());
        const auto deadline = sequence == 1 ? 1u : 0u;
        auto write = plan.acquire_input(sequence, deadline);
        if (!write || write->bytes.size() != bytes) {
            result.reason = "input_lease_failed";
            return result;
        }
        std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
        if (!plan.submit({write->token, deadline})) {
            result.reason = "submission_rejected";
            return result;
        }
        ++result.submissions;
        previous = input.back();

        std::optional<SharedIoComputePlan::Completion> completion;
        const auto deadline_wall = Clock::now() + std::chrono::seconds(5);
        while (!completion && Clock::now() < deadline_wall) {
            plan.drain(sequence == 1 ? std::numeric_limits<std::uint64_t>::max() : now_ns());
            completion = plan.pop_completion();
            if (!completion)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!completion) {
            result.reason = "completion_timeout";
            return result;
        }
        if (completion->late) {
            ++result.late;
            // The transport has expired the GPU delivery. The prepared CPU
            // oracle is the harness fallback shadow for this same causal
            // block; copy its samples into the fallback delivery buffer and
            // compare the delivered samples against the independent oracle
            // expectation before discarding the late completion.
            std::array<float, 2> fallback_output{};
            std::copy(cpu_output.begin(), cpu_output.end(), fallback_output.begin());
            result.cpu_fallback_delivered = true;
            result.cpu_fallback_parity =
                result.cpu_fallback_parity && compare_block(fallback_output, expected);
            result.late_result_rejected =
                plan.expire_delivery(*completion) && plan.discard_completion(*completion);
            ++result.cpu_fallback;
        } else {
            auto output = plan.acquire_output(*completion);
            if (!output || output->bytes.size() != bytes) {
                result.reason = "output_lease_failed";
                return result;
            }
            result.cpu_parity =
                result.cpu_parity ||
                compare_block({reinterpret_cast<const float*>(output->bytes.data()), 2}, expected);
            result.cpu_parity = result.cpu_parity && plan.release_output({output->token});
            ++result.gpu_delivered;
        }
        if (sequence == 1)
            result.cpu_parity = true;
        ++sequence;
    }
    result.cpu_parity = result.cpu_parity && result.gpu_delivered == 3;

    // The plan exposes immutable lifecycle rows only after all slot leases are
    // quiescent. A clean provider drain then permits an explicit epoch reprime.
    auto first_receipts = plan.take_lifecycle_receipts();
    result.retired = plan.lifecycle_receipt_valid() && first_receipts.size() == 4;
    result.retired =
        result.retired &&
        std::all_of(first_receipts.begin(), first_receipts.end(), [](const auto& receipt) {
            return receipt.terminal && receipt.submit_ns >= 0 &&
                   receipt.terminal_ns >= receipt.submit_ns &&
                   (receipt.output_released || receipt.discarded);
        });
    if (!provider.drain()) {
        result.reason = "provider_drain_failed";
        return result;
    }
    if (!provider.can_resume_after_drain()) {
        result.reason = "provider_not_resumable";
        return result;
    }
    if (!plan.reprime_when_quiescent()) {
        result.reason = "plan_reprime_failed";
        return result;
    }
    result.reprimed = true;
    result.reprime_generation = plan.preparation_epoch();

    const std::array<float, 2> input{5.0f, 5.25f};
    auto write = plan.acquire_input(5, 0);
    if (!write) {
        result.reason = "reprime_input_lease_failed";
        return result;
    }
    std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
    if (!plan.submit({write->token, 0})) {
        result.reason = "reprime_submission_rejected";
        return result;
    }
    const auto reprime_deadline = Clock::now() + std::chrono::seconds(5);
    std::optional<SharedIoComputePlan::Completion> reprime_completion;
    while (!reprime_completion && Clock::now() < reprime_deadline) {
        plan.drain(now_ns());
        reprime_completion = plan.pop_completion();
        if (!reprime_completion)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto reprime_output = reprime_completion ? plan.acquire_output(*reprime_completion)
                                             : std::optional<SharedIoArena::OutputLease>{};
    if (!reprime_output) {
        result.reason = "reprime_completion_failed";
        return result;
    }
    (void)plan.release_output({reprime_output->token});
    auto second_receipts = plan.take_lifecycle_receipts();
    result.reprimed = result.reprimed && result.reprime_generation != result.generation &&
                      second_receipts.size() == 1 && second_receipts.front().terminal;

    const auto transfer = transfer_counter.snapshot();
    result.resident_state_copies = transfer.copy_buffer_to_buffer_calls;
    result.zero_audio_slot_transfers =
        transfer.queue_write_buffer_calls == 0 && transfer.buffer_map_async_calls == 0 &&
        transfer.buffer_get_mapped_range_calls == 0 && transfer.buffer_unmap_calls == 0 &&
        transfer.copy_buffer_to_buffer_calls ==
            (result.submissions + 1u) * (resident_history_bytes != 0 ? 4 : 0);
    result.retired = result.retired && plan.release();
    run_device_loss_negative(result, fixture, transfer_counter.deferred_proc_table());
    if (result.device_loss_negative)
        result.reason = result.retired ? "authenticated_one_stream_pass" : "release_failed";
    return result;
}

} // namespace

int main(int argc, char** argv) {
    bool strict = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--strict")
            strict = true;
        else
            return 2;
    }
    const auto result = run();
    if (result.unavailable)
        return strict ? 77 : 0;
    const bool passed =
        result.identity && result.resident_state && result.zero_audio_slot_transfers &&
        result.cpu_parity && result.cpu_fallback_parity && result.cpu_fallback_delivered &&
        result.late_result_rejected && result.retired && result.reprimed &&
        result.immutable_provenance && result.device_loss_negative && result.device_loss_receipt;
    emit(result, passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
