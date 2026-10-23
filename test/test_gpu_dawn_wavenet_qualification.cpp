#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/gpu_convolver_trial_config.hpp"
#include "detail/gpu_neural_model.hpp"
#include "detail/nam_tcn_artifact.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_wavenet_spec.hpp"
#include "detail/wavenet_realtime_channel.hpp"
#include "detail/wavenet_streaming_backend.hpp"
#include "support/dawn_transfer_call_counter.hpp"
#include "support/physical_qualification_outcome.hpp"
#include "support/wavenet_program_session_channel.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>

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

std::string_view recovery_name(pulp::gpu_audio::GpuAudioRecoveryReason reason) {
    using R = pulp::gpu_audio::GpuAudioRecoveryReason;
    switch (reason) {
    case R::None:
        return "none";
    case R::SequenceGap:
        return "sequence_gap";
    case R::InputSaturated:
        return "input_saturated";
    case R::ProviderFailure:
        return "provider_failure";
    case R::ProviderLost:
        return "provider_lost";
    case R::OfflineFence:
        return "offline_fence";
    case R::InvalidCallback:
        return "invalid_callback";
    case R::ProviderUnresponsive:
        return "provider_unresponsive";
    case R::ProviderReacquired:
        return "provider_reacquired";
    case R::SustainedDeadlineMisses:
        return "sustained_deadline_misses";
    }
    return "unknown";
}

// One admission-time scenario: the stamped WaveNet node over a physical
// provider for a fixed number of callbacks. `skip_pump_at` withholds the worker
// pumps after one callback, so the block it admitted is still in flight when
// the next admission needs a slot and must take slot 1.
struct AdmissionRun {
    bool ran = false;
    std::uint64_t callbacks = 0;
    std::uint64_t next_sequence = 0;
    // Callbacks after which the bridge's next sequence was not one past the
    // callback's own.
    std::uint64_t sequence_drift = 0;
    std::uint64_t gpu_blocks = 0;
    std::uint64_t mismatches = 0;
    // Submissions the provider refused, as the node's channel saw them.
    std::uint64_t refused_submits = 0;
    std::uint64_t first_refused = UINT64_MAX;
    pulp::gpu_audio::GpuAudioRecoveryReason reason = pulp::gpu_audio::GpuAudioRecoveryReason::None;
    std::uint64_t sequence = UINT64_MAX;
    std::uint64_t latched_after = UINT64_MAX;
    pulp::gpu_audio::GpuAudioProviderState state =
        pulp::gpu_audio::GpuAudioProviderState::Uninitialized;
    bool statuses_valid = true;
    bool released = false;
    std::string trace;
    std::string error;

    bool sequence_agrees() const noexcept {
        return ran && sequence_drift == 0 && next_sequence == callbacks;
    }
};

struct AdmissionScenario {
    DawnSharedIoProvider::Fault fault = DawnSharedIoProvider::Fault::None;
    std::uint32_t fault_slot = 0;
    std::uint32_t lead = 1;
    std::uint32_t inflight = 1;
    std::uint64_t skip_pump_at = UINT64_MAX;
};

// Records the submissions the provider refused on their way from the node to
// the physical channel, so a receipt can tie a latch to the refused block.
class RefusalCountingChannel final : public WaveNetRealtimeChannel {
  public:
    RefusalCountingChannel(std::unique_ptr<WaveNetRealtimeChannel> inner, AdmissionRun& run)
        : inner_(std::move(inner)), run_(run) {}
    bool submit(std::span<const float> input,
                const SharedIoSubmissionTicket& ticket) noexcept override {
        if (inner_->submit(input, ticket))
            return true;
        if (run_.refused_submits++ == 0)
            run_.first_refused = ticket.sequence;
        return false;
    }
    void service(std::uint64_t now) noexcept override {
        inner_->service(now);
    }
    void service_until(std::uint64_t now, std::uint64_t deadline) noexcept override {
        inner_->service_until(now, deadline);
    }
    std::optional<WaveNetChannelCompletion> receive(std::span<float> output) noexcept override {
        return inner_->receive(output);
    }
    bool release() noexcept override {
        return inner_->release();
    }
    bool release_bounded(std::uint64_t budget_ns) noexcept override {
        return inner_->release_bounded(budget_ns);
    }
    bool lost() const noexcept override {
        return inner_->lost();
    }

  private:
    std::unique_ptr<WaveNetRealtimeChannel> inner_;
    AdmissionRun& run_;
};

struct RefusalRetryRun {
    bool refused = false;
    bool retried = false;
    std::uint64_t delivered = 0;
    std::uint64_t mismatches = 0;
    std::string error;
};

void emit_admission(std::ostream& out, std::string_view name, const AdmissionRun& run,
                    bool receipt) {
    const auto signed_sequence = [](std::uint64_t value) {
        return value == UINT64_MAX ? -1LL : static_cast<long long>(value);
    };
    out << R"(,")" << name << R"(":{"receipt":)" << (receipt ? "true" : "false")
        << R"(,"sequence_agrees":)" << (run.sequence_agrees() ? "true" : "false")
        << R"(,"callbacks":)" << run.callbacks << R"(,"bridge_next_sequence":)" << run.next_sequence
        << R"(,"sequence_drift":)" << run.sequence_drift << R"(,"gpu_blocks":)" << run.gpu_blocks
        << R"(,"gpu_mismatches":)" << run.mismatches << R"(,"refused_submits":)"
        << run.refused_submits << R"(,"first_refused_sequence":)"
        << signed_sequence(run.first_refused) << R"(,"recovery_reason":")"
        << recovery_name(run.reason) << R"(","recovery_sequence":)" << signed_sequence(run.sequence)
        << R"(,"latched_after_callback":)" << signed_sequence(run.latched_after)
        << R"(,"provider_state":)" << static_cast<int>(run.state) << R"(,"statuses_valid":)"
        << (run.statuses_valid ? "true" : "false") << R"(,"released":)"
        << (run.released ? "true" : "false") << R"(,"trace":")" << run.trace << R"(","error":")"
        << run.error << R"("})";
}

struct Result {
    bool unavailable = false;
    bool identity = false;
    bool resident_state = false;
    bool history_transfer_contract = false;
    bool zero_audio_slot_transfers = false;
    bool cpu_parity = false;
    bool cpu_fallback_parity = false;
    bool cpu_fallback_delivered = false;
    // The forced-late branch is a harness shadow delivery. It proves oracle
    // parity and disposition handling, but it is not physical provider
    // fallback delivery evidence.
    bool cpu_fallback_physical_proof = false;
    bool late_result_rejected = false;
    bool retired = false;
    bool reprimed = false;
    bool immutable_provenance = false;
    bool device_loss_negative = false;
    bool device_loss_receipt = false;
    // Node-level reasons from the stamped realtime node over a physical
    // provider with an injected fault: loss after completion registration,
    // and a slot whose completion is never serviced.
    bool node_loss_receipt = false;
    bool node_hang_receipt = false;
    std::string node_loss_reason = "not_run";
    std::string node_hang_reason = "not_run";
    std::uint64_t node_loss_sequence = 0;
    std::uint64_t node_hang_sequence = 0;
    std::uint64_t node_loss_blocks = 0;
    std::uint64_t node_hang_blocks = 0;
    bool neural_loss_receipt = false;
    bool neural_hang_receipt = false;
    std::string neural_loss_reason = "not_run";
    std::string neural_hang_reason = "not_run";
    std::uint64_t neural_loss_sequence = 0;
    std::uint64_t neural_hang_sequence = 0;
    std::uint64_t neural_loss_blocks = 0;
    std::uint64_t neural_hang_blocks = 0;
    // Re-acquire after the same faults: a new physical provider replaces the
    // faulted one while the stream continues, and GPU delivery resumes.
    bool reacquire_loss_receipt = false;
    bool reacquire_hang_receipt = false;
    std::uint64_t reacquire_loss_resume_ns = 0;
    std::uint64_t reacquire_hang_resume_ns = 0;
    std::uint64_t reacquire_loss_reprepare_ns = 0;
    std::uint64_t reacquire_hang_reprepare_ns = 0;
    std::string reacquire_loss_reason = "not_run";
    std::string reacquire_hang_reason = "not_run";
    // Admission-time faults on the stamped WaveNet node over a physical
    // provider, each with the bridge's sequence checked against the callbacks.
    AdmissionRun admission_healthy;
    AdmissionRun admission_loss;
    AdmissionRun admission_reject;
    AdmissionRun admission_skip_control;
    AdmissionRun admission_reject_midstream;
    bool admission_healthy_receipt = false;
    bool admission_loss_receipt = false;
    bool admission_reject_receipt = false;
    AdmissionRun admission_reject_unreachable_slot;
    bool admission_reject_unreachable_slot_receipt = false;
    bool admission_skip_control_receipt = false;
    bool admission_reject_midstream_receipt = false;
    RefusalRetryRun refusal_retry;
    bool refusal_retry_receipt = false;
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
              << R"(,"history_transfer_contract":)"
              << (result.history_transfer_contract ? "true" : "false")
              << R"(,"zero_audio_slot_transfers":)"
              << (result.zero_audio_slot_transfers ? "true" : "false") << R"(,"cpu_parity":)"
              << (result.cpu_parity ? "true" : "false") << R"(,"cpu_fallback_parity":)"
              << (result.cpu_fallback_parity ? "true" : "false") << R"(,"cpu_fallback_delivered":)"
              << (result.cpu_fallback_delivered ? "true" : "false")
              << R"(,"cpu_fallback_physical_proof":)"
              << (result.cpu_fallback_physical_proof ? "true" : "false")
              << R"(,"cpu_fallback_delivery_mode":"shadow_only")"
              << R"(,"continuous_cpu_fallback":)" << (result.cpu_fallback != 0 ? "true" : "false")
              << R"(,"late_result_rejected":)" << (result.late_result_rejected ? "true" : "false")
              << R"(,"retired":)" << (result.retired ? "true" : "false") << R"(,"reprimed":)"
              << (result.reprimed ? "true" : "false") << R"(,"immutable_provenance":)"
              << (result.immutable_provenance ? "true" : "false") << R"(,"device_loss_negative":)"
              << (result.device_loss_negative ? "true" : "false") << R"(,"device_loss_receipt":)"
              << (result.device_loss_receipt ? "true" : "false") << R"(,"node_loss_receipt":)"
              << (result.node_loss_receipt ? "true" : "false") << R"(,"node_loss_reason":")"
              << result.node_loss_reason << R"(","node_loss_sequence":)"
              << result.node_loss_sequence << R"(,"node_loss_blocks":)" << result.node_loss_blocks
              << R"(,"node_hang_receipt":)" << (result.node_hang_receipt ? "true" : "false")
              << R"(,"node_hang_reason":")" << result.node_hang_reason
              << R"(","node_hang_sequence":)" << result.node_hang_sequence
              << R"(,"node_hang_blocks":)" << result.node_hang_blocks
              << R"(,"neural_loss_receipt":)" << (result.neural_loss_receipt ? "true" : "false")
              << R"(,"neural_loss_reason":")" << result.neural_loss_reason
              << R"(","neural_loss_sequence":)" << result.neural_loss_sequence
              << R"(,"neural_loss_blocks":)" << result.neural_loss_blocks
              << R"(,"neural_hang_receipt":)" << (result.neural_hang_receipt ? "true" : "false")
              << R"(,"neural_hang_reason":")" << result.neural_hang_reason
              << R"(","neural_hang_sequence":)" << result.neural_hang_sequence
              << R"(,"neural_hang_blocks":)" << result.neural_hang_blocks
              << R"(,"reacquire_loss_receipt":)"
              << (result.reacquire_loss_receipt ? "true" : "false")
              << R"(,"reacquire_loss_reason":")" << result.reacquire_loss_reason
              << R"(","reacquire_loss_reprepare_ns":)" << result.reacquire_loss_reprepare_ns
              << R"(,"reacquire_loss_resume_ns":)" << result.reacquire_loss_resume_ns
              << R"(,"reacquire_hang_receipt":)"
              << (result.reacquire_hang_receipt ? "true" : "false")
              << R"(,"reacquire_hang_reason":")" << result.reacquire_hang_reason
              << R"(","reacquire_hang_reprepare_ns":)" << result.reacquire_hang_reprepare_ns
              << R"(,"reacquire_hang_resume_ns":)" << result.reacquire_hang_resume_ns;
    emit_admission(std::cout, "admission_healthy", result.admission_healthy,
                   result.admission_healthy_receipt);
    emit_admission(std::cout, "admission_force_loss_before_submit", result.admission_loss,
                   result.admission_loss_receipt);
    emit_admission(std::cout, "admission_reject_before_submit", result.admission_reject,
                   result.admission_reject_receipt);
    emit_admission(std::cout, "admission_reject_unreachable_slot",
                   result.admission_reject_unreachable_slot,
                   result.admission_reject_unreachable_slot_receipt);
    emit_admission(std::cout, "admission_skip_pump_control", result.admission_skip_control,
                   result.admission_skip_control_receipt);
    emit_admission(std::cout, "admission_reject_midstream", result.admission_reject_midstream,
                   result.admission_reject_midstream_receipt);
    std::cout << R"(,"refusal_retry":{"receipt":)"
              << (result.refusal_retry_receipt ? "true" : "false") << R"(,"refused_on_slot1":)"
              << (result.refusal_retry.refused ? "true" : "false") << R"(,"retried_on_slot0":)"
              << (result.refusal_retry.retried ? "true" : "false") << R"(,"delivered":)"
              << result.refusal_retry.delivered << R"(,"gpu_mismatches":)"
              << result.refusal_retry.mismatches << R"(,"error":")" << result.refusal_retry.error
              << R"("})" << R"(,"submissions":)" << result.submissions << R"(,"gpu_delivered":)"
              << result.gpu_delivered << R"(,"cpu_fallback_blocks":)" << result.cpu_fallback
              << R"(,"late_blocks":)" << result.late << R"(,"resident_state_copies":)"
              << result.resident_state_copies << R"(,"generation":)" << result.generation
              << R"(,"reprime_generation":)" << result.reprime_generation << R"(,"reason":")"
              << result.reason << "\"}" << '\n';
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

struct NodeFaultRun {
    bool ran = false;
    pulp::gpu_audio::GpuAudioRecoveryReason reason = pulp::gpu_audio::GpuAudioRecoveryReason::None;
    pulp::gpu_audio::GpuAudioProviderState state =
        pulp::gpu_audio::GpuAudioProviderState::Uninitialized;
    std::uint64_t sequence = 0;
    std::uint64_t blocks = 0;
    bool status_valid = false;
    std::string reason_text;
    // Re-acquire, when requested.
    bool reacquired = false;
    bool resumed_with_parity = false;
    std::uint64_t reprepare_ns = 0;
    std::uint64_t resume_ns = 0;
    std::uint64_t epoch = 0;
};

// Builds a session over a fresh, healthy physical provider for a re-acquire.
struct PhysicalChannelFactory {
    const Fixture* fixture = nullptr;
    const void* proc_table = nullptr;
    int created = 0;
    static std::unique_ptr<WaveNetRealtimeChannel> make(void* context) noexcept {
        auto& self = *static_cast<PhysicalChannelFactory*>(context);
        auto created = DawnSharedIoProvider::create(
            {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
             .proc_table_override_for_testing = self.proc_table,
             .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
        if (!created.provider)
            return nullptr;
        auto program = DawnSharedIoWavenetProgram::create(*created.provider, self.fixture->spec());
        if (!program)
            return nullptr;
        const auto bytes = self.fixture->spec().block_size * sizeof(float);
        auto session = std::make_unique<SharedIoProgramSession>();
        if (!session->prepare(
                {std::move(created.provider), std::move(program)},
                {.slots = 2,
                 .input_bytes_per_slot = bytes,
                 .output_bytes_per_slot = bytes,
                 .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer}))
            return nullptr;
        ++self.created;
        return std::make_unique<pulp::test::ProgramSessionChannel>(std::move(session));
    }
};

// Drives the stamped node over a physical provider with `fault` until a
// recovery reason latches. Callbacks and worker pumps alternate on this thread
// with a short real wait, so the provider sees real device time.
NodeFaultRun run_node_fault(const Fixture& fixture, const void* proc_table,
                            DawnSharedIoProvider::Fault fault, bool reacquire = false) {
    namespace ga = pulp::gpu_audio;
    NodeFaultRun run;
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = proc_table,
         .fault = fault,
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider) {
        run.reason_text = "provider_unavailable";
        return run;
    }
    auto program = DawnSharedIoWavenetProgram::create(*created.provider, fixture.spec());
    if (!program) {
        run.reason_text = "program_unavailable";
        return run;
    }
    const auto bytes = fixture.spec().block_size * sizeof(float);
    auto session = std::make_unique<SharedIoProgramSession>();
    if (!session->prepare(
            {std::move(created.provider), std::move(program)},
            {.slots = 2,
             .input_bytes_per_slot = bytes,
             .output_bytes_per_slot = bytes,
             .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer})) {
        run.reason_text = "session_prepare_failed";
        return run;
    }
    const std::array<std::uint32_t, 1> dilations{1};
    const ga::GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                              .condition_size = 1,
                                              .channels = 1,
                                              .kernel = 2,
                                              .head_size = 1,
                                              .tanh_activation = true,
                                              .dilations = dilations};
    ga::GpuWaveNetRealtimeNode::Config config{
        .session = {.descriptor = {.block_size = fixture.spec().block_size,
                                   .sample_rate = 48000,
                                   .stream_instances = 1,
                                   .layers = {&layer, 1},
                                   .weight_count = fixture.weights.size()},
                    .weights = fixture.weights,
                    .slots = 2},
        .channels = 1,
        .lead_blocks = 1,
        .capacity = 4,
        .max_inflight = 1,
        // Hang detection within this receipt's short run: the ring-sized
        // window. The time-based default has its own campaign receipt.
        .watchdog_blocks = 3};
    ga::GpuWaveNetRealtimeNode node(config);
    std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels;
    channels.push_back(std::make_unique<pulp::test::ProgramSessionChannel>(std::move(session)));
    if (!WaveNetRealtimeTestAccess::prepare(node, std::move(channels))) {
        run.reason_text = "node_prepare_failed";
        return run;
    }
    PhysicalChannelFactory factory{.fixture = &fixture, .proc_table = proc_table};
    if (reacquire)
        WaveNetRealtimeTestAccess::set_channel_factory(node, &PhysicalChannelFactory::make,
                                                       &factory);
    auto path = realtime_gpu_node_path(&node);
    std::array<float, 2> input{}, output{};
    const float* in_ptr = input.data();
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in(&in_ptr, 1, 2);
    pulp::audio::BufferView<float> out(&out_ptr, 1, 2);
    run.ran = true;
    // The fixture computes y[n] = tanh(0.5 x[n-1]); block B's input is
    // {B, B + 0.25}, so a delivered block has a closed-form expectation.
    const auto expected_block = [](std::uint64_t block) {
        const float previous = block == 0 ? 0.0f : static_cast<float>(block - 1) + 0.25f;
        return std::array<float, 2>{std::tanh(0.5f * previous),
                                    std::tanh(0.5f * static_cast<float>(block))};
    };
    std::uint64_t fence_ns = 0, resumed_blocks = 0;
    bool parity = true;
    for (std::uint64_t sequence = 0; sequence < 256; ++sequence) {
        input = {static_cast<float>(sequence), static_cast<float>(sequence) + 0.25f};
        const auto status = path.process(path.context, in, out, 2, sequence, true, now_ns());
        const auto disposition =
            status == kRealtimeGpuReady     ? SharedIoDeliveryDisposition::GpuDelivered
            : status == kRealtimeGpuPriming ? SharedIoDeliveryDisposition::Priming
                                            : SharedIoDeliveryDisposition::SilenceDelivered;
        path.delivered(path.context, sequence, static_cast<std::uint8_t>(disposition), 0, 0);
        ++run.blocks;
        if (run.reacquired && status == kRealtimeGpuReady) {
            if (resumed_blocks++ == 0)
                run.resume_ns = now_ns() - fence_ns;
            parity = parity && compare_block(output, expected_block(sequence - 1));
            if (resumed_blocks == 8)
                break;
        }
        for (int pump = 0; pump < 4; ++pump) {
            (void)path.service(path.context, 0);
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        if (!run.reacquired && node.recovery_reason() != ga::GpuAudioRecoveryReason::None) {
            run.reason = node.recovery_reason();
            run.sequence = node.recovery_sequence();
            const auto fenced = node.status_snapshot();
            run.state = fenced.provider_state;
            run.status_valid = ga::validate_gpu_audio_status(fenced).accepted() &&
                               fenced.selected_engine != ga::GpuAudioEngine::Gpu;
            if (!reacquire)
                break;
            fence_ns = now_ns();
            run.reacquired = path.reprepare(path.context, 250'000'000);
            run.reprepare_ns = now_ns() - fence_ns;
            if (!run.reacquired)
                break;
        }
    }
    run.resumed_with_parity = run.reacquired && resumed_blocks == 8 && parity;
    if (run.reacquired)
        run.epoch = node.status_snapshot().epoch;
    const auto status = node.status_snapshot();
    if (reacquire)
        run.status_valid = run.status_valid && ga::validate_gpu_audio_status(status).accepted() &&
                           status.recovery_reason == ga::GpuAudioRecoveryReason::ProviderReacquired;
    else
        run.state = status.provider_state;
    run.reason_text = std::string(recovery_name(run.reason));
    if (!node.release())
        run.reason_text += ":release_retained";
    return run;
}

// A GpuWaveNetSession stand-in whose create() hands out a program session
// prepared over a faulted physical provider, so the backend reaches it through
// the same create path as the public session.
struct FaultedSession {
    struct CreateResult {
        std::unique_ptr<FaultedSession> session;
        pulp::gpu_audio::GpuWaveNetSessionError error =
            pulp::gpu_audio::GpuWaveNetSessionError::ProviderUnavailable;
        explicit operator bool() const noexcept {
            return session != nullptr && error == pulp::gpu_audio::GpuWaveNetSessionError::None;
        }
    };
    static inline std::unique_ptr<SharedIoProgramSession> next;
    static CreateResult create(const pulp::gpu_audio::GpuWaveNetSession::Config&) {
        if (!next)
            return {};
        auto session = std::make_unique<FaultedSession>();
        session->channel = std::make_unique<pulp::test::ProgramSessionChannel>(std::move(next));
        return {std::move(session), pulp::gpu_audio::GpuWaveNetSessionError::None};
    }
    std::unique_ptr<pulp::test::ProgramSessionChannel> channel;
    bool submit_block(std::span<const float> input, std::uint64_t sequence) noexcept {
        // The outer session channel keeps the node's ticket; this shim only
        // needs the block's sequence.
        return channel->submit(input, admit_submission(0, sequence));
    }
    std::size_t service(std::uint64_t now) noexcept {
        channel->service(now);
        return 0;
    }
    std::size_t service_until(std::uint64_t deadline) noexcept {
        channel->service_until(now_ns(), deadline);
        return 0;
    }
    std::optional<pulp::gpu_audio::GpuWaveNetBlockResult>
    receive(std::span<float> output) noexcept {
        return channel->receive_result(output);
    }
    bool release() noexcept {
        return channel->release();
    }
    bool device_lost() const noexcept {
        return channel->lost();
    }
};

// The same physical faults behind the generic neural node: its WaveNet
// backend creates its one session through FaultedSession, and its CPU model
// is a silent stand-in, since only the latched recovery is under test.
NodeFaultRun run_neural_node_fault(const Fixture& fixture, const void* proc_table,
                                   DawnSharedIoProvider::Fault fault) {
    namespace ga = pulp::gpu_audio;
    NodeFaultRun run;
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = proc_table,
         .fault = fault,
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider) {
        run.reason_text = "provider_unavailable";
        return run;
    }
    auto program = DawnSharedIoWavenetProgram::create(*created.provider, fixture.spec());
    if (!program) {
        run.reason_text = "program_unavailable";
        return run;
    }
    const auto block = fixture.spec().block_size;
    const auto bytes = block * sizeof(float);
    auto session = std::make_unique<SharedIoProgramSession>();
    if (!session->prepare(
            {std::move(created.provider), std::move(program)},
            {.slots = 2,
             .input_bytes_per_slot = bytes,
             .output_bytes_per_slot = bytes,
             .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer})) {
        run.reason_text = "session_prepare_failed";
        return run;
    }
    const std::array<std::uint32_t, 1> dilations{1};
    const ga::GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                              .condition_size = 1,
                                              .channels = 1,
                                              .kernel = 2,
                                              .head_size = 1,
                                              .tanh_activation = true,
                                              .dilations = dilations};
    auto backend = std::make_unique<WaveNetStreamingBackend>(
        WaveNetStreamingBackend::Config{
            .session = {.descriptor = {.block_size = block,
                                       .sample_rate = 48000,
                                       .stream_instances = 1,
                                       .layers = {&layer, 1},
                                       .weight_count = fixture.weights.size()},
                        .weights = fixture.weights,
                        .slots = 2},
            .channels = 1,
            .max_inflight = 1},
        WaveNetStreamingBackend::ChannelFactory{}, &create_session_channel<FaultedSession>);
    FaultedSession::next = std::move(session);
    int silent_state = 0;
    const NeuralCpuKernel silent{
        .state = &silent_state,
        .prepare = [](void*, const StreamingPrepareContext&) noexcept { return true; },
        .process = [](void*, const float*, float* out,
                      std::uint32_t n) noexcept { std::fill_n(out, n, 0.0f); },
        .reset = [](void*) noexcept {},
        .quiesce = [](void*) noexcept { return true; },
        .release = [](void*) noexcept { return true; }};
    NeuralModelRequest request;
    request.family = ga::NeuralFamily::WaveNet;
    request.manifest = {.model_id = "pulp.qualification.wavenet",
                        .architecture = "wavenet",
                        .model_version = "1",
                        .artifact_id = "pulp.qualification.wavenet.weights",
                        .artifact_sha256 =
                            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                        .license = "MIT",
                        .runtime = "pulp.qualification.v1",
                        .artifact_size_bytes = 64,
                        .sample_rate_hz = 48000,
                        .redistributable = true};
    request.block_size = block;
    request.channel_kernels = {&silent, 1};
    request.backend = std::move(backend);
    auto build = make_neural_model(std::move(request));
    if (!build) {
        run.reason_text = "model_rejected";
        return run;
    }
    ga::GpuNeuralRealtimeNode node(
        {.family = ga::NeuralFamily::WaveNet,
         .model = build.model,
         .channels = 1,
         .lead_blocks = 1,
         .capacity = 4,
         .shadow_policy = ga::GpuNeuralRealtimeNode::ShadowPolicy::Inline,
         // As run_node_fault: the ring-sized window for a short run.
         .watchdog_blocks = 3});
    // The fault must hit a node that is on the GPU, not one that fell back to
    // its CPU model. (The injected provider is not the authenticated one, so
    // the status says Degraded; the strict Ready receipt is the public one.)
    if (!node.prepare() || !node.gpu_kernel_available() || node.epoch() == 0 ||
        node.recovery_reason() != ga::GpuAudioRecoveryReason::None) {
        run.reason_text = "node_not_on_gpu";
        return run;
    }
    auto path = realtime_gpu_node_path(&node);
    std::array<float, 2> input{}, output{};
    const float* in_ptr = input.data();
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in(&in_ptr, 1, 2);
    pulp::audio::BufferView<float> out(&out_ptr, 1, 2);
    run.ran = true;
    for (std::uint64_t sequence = 0; sequence < 64; ++sequence) {
        input = {static_cast<float>(sequence), static_cast<float>(sequence) + 0.25f};
        const auto status = path.process(path.context, in, out, 2, sequence, true, now_ns());
        const auto disposition =
            status == kRealtimeGpuReady     ? SharedIoDeliveryDisposition::GpuDelivered
            : status == kRealtimeGpuPriming ? SharedIoDeliveryDisposition::Priming
                                            : SharedIoDeliveryDisposition::CpuFallbackDelivered;
        path.delivered(path.context, sequence, static_cast<std::uint8_t>(disposition), 0, 0);
        ++run.blocks;
        for (int pump = 0; pump < 4; ++pump) {
            (void)path.service(path.context, 0);
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        if (node.recovery_reason() != ga::GpuAudioRecoveryReason::None)
            break;
    }
    run.reason = node.recovery_reason();
    run.sequence = node.recovery_sequence();
    const auto status = node.status_snapshot();
    run.state = status.provider_state;
    run.status_valid = ga::validate_gpu_audio_status(status).accepted() &&
                       status.selected_engine != ga::GpuAudioEngine::Gpu;
    run.reason_text = std::string(recovery_name(run.reason));
    if (!node.release())
        run.reason_text += ":release_retained";
    return run;
}

constexpr std::uint64_t kAdmissionCallbacks = 24;
// Each harness callback block takes about a millisecond (four pumps with a
// 250 us wait), far longer than the fixture's two-sample period, so the default
// watchdog of capacity - 1 blocks would call a few milliseconds of host
// scheduling delay an unresponsive provider. These runs test admission faults,
// so the window and the ring are wide enough that only a real stall latches.
constexpr std::uint32_t kAdmissionCapacity = 16;
constexpr std::uint32_t kAdmissionWatchdogBlocks = 12;

AdmissionRun run_admission(const Fixture& fixture, const void* proc_table,
                           const AdmissionScenario& scenario) {
    namespace ga = pulp::gpu_audio;
    AdmissionRun run;
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = proc_table,
         .fault = scenario.fault,
         .fault_slot = scenario.fault_slot,
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider) {
        run.error = "provider_unavailable";
        return run;
    }
    auto program = DawnSharedIoWavenetProgram::create(*created.provider, fixture.spec());
    const auto bytes = fixture.spec().block_size * sizeof(float);
    auto session = std::make_unique<SharedIoProgramSession>();
    if (!program ||
        !session->prepare(
            {std::move(created.provider), std::move(program)},
            {.slots = 2,
             .input_bytes_per_slot = bytes,
             .output_bytes_per_slot = bytes,
             .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer})) {
        run.error = "session_prepare_failed";
        return run;
    }
    const std::array<std::uint32_t, 1> dilations{1};
    const ga::GpuWaveNetLayerDescriptor layer{.input_size = 1,
                                              .condition_size = 1,
                                              .channels = 1,
                                              .kernel = 2,
                                              .head_size = 1,
                                              .tanh_activation = true,
                                              .dilations = dilations};
    ga::GpuWaveNetRealtimeNode::Config config{
        .session = {.descriptor = {.block_size = fixture.spec().block_size,
                                   .sample_rate = 48000,
                                   .stream_instances = 1,
                                   .layers = {&layer, 1},
                                   .weight_count = fixture.weights.size()},
                    .weights = fixture.weights,
                    .slots = 2},
        .channels = 1,
        .lead_blocks = scenario.lead,
        .capacity = kAdmissionCapacity,
        .max_inflight = scenario.inflight,
        .watchdog_blocks = kAdmissionWatchdogBlocks};
    ga::GpuWaveNetRealtimeNode node(config);
    std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels;
    channels.push_back(std::make_unique<RefusalCountingChannel>(
        std::make_unique<pulp::test::ProgramSessionChannel>(std::move(session)), run));
    if (!WaveNetRealtimeTestAccess::prepare(node, std::move(channels))) {
        run.error = "node_prepare_failed";
        return run;
    }
    auto path = realtime_gpu_node_path(&node);
    std::array<float, 2> input{}, output{};
    const float* in_ptr = input.data();
    float* out_ptr = output.data();
    pulp::audio::BufferView<const float> in(&in_ptr, 1, 2);
    pulp::audio::BufferView<float> out(&out_ptr, 1, 2);
    run.ran = true;
    for (std::uint64_t sequence = 0; sequence < kAdmissionCallbacks; ++sequence) {
        input = {static_cast<float>(sequence), static_cast<float>(sequence) + 0.25f};
        const auto status = path.process(path.context, in, out, 2, sequence, true, now_ns());
        const auto disposition =
            status == kRealtimeGpuReady     ? SharedIoDeliveryDisposition::GpuDelivered
            : status == kRealtimeGpuPriming ? SharedIoDeliveryDisposition::Priming
                                            : SharedIoDeliveryDisposition::SilenceDelivered;
        path.delivered(path.context, sequence, static_cast<std::uint8_t>(disposition), 0, 0);
        ++run.callbacks;
        if (path.next_sequence(path.context) != sequence + 1)
            ++run.sequence_drift;
        run.trace += status == kRealtimeGpuReady ? 'G' : status == kRealtimeGpuPriming ? 'P' : 'S';
        if (status == kRealtimeGpuReady) {
            // y[n] = tanh(0.5 x[n-1]) over the whole stream, so every GPU
            // block also checks the history carried in from the block before.
            ++run.gpu_blocks;
            const auto due = sequence - scenario.lead;
            const float previous = due == 0 ? 0.0f : static_cast<float>(due - 1) + 0.25f;
            const std::array<float, 2> expected{std::tanh(0.5f * previous),
                                                std::tanh(0.5f * static_cast<float>(due))};
            if (!compare_block(output, expected))
                ++run.mismatches;
        }
        for (int pump = 0; sequence != scenario.skip_pump_at && pump < 4; ++pump) {
            (void)path.service(path.context, 0);
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        run.statuses_valid =
            run.statuses_valid && ga::validate_gpu_audio_status(node.status_snapshot()).accepted();
        if (run.latched_after == UINT64_MAX &&
            node.recovery_reason() != ga::GpuAudioRecoveryReason::None)
            run.latched_after = sequence;
    }
    const auto status = node.status_snapshot();
    run.reason = status.recovery_reason;
    run.sequence = status.recovery_sequence;
    run.state = status.provider_state;
    run.statuses_valid = run.statuses_valid && ga::validate_gpu_audio_status(status).accepted();
    run.next_sequence = path.next_sequence(path.context);
    run.released = node.release();
    return run;
}

// A refused submission must not move the device-resident stream position.
// Blocks 0..9 run one at a time on slot 0; block 10 is left in flight so block
// 11 lands on the refusing slot 1; block 11 is then retried on slot 0 and the
// stream continues. Every delivered block must match the whole-stream CPU
// reference, so a position that advanced on the refusal shows as a one-block
// history skew from block 11 on.
RefusalRetryRun run_refusal_retry(const Fixture& fixture, const void* proc_table) {
    RefusalRetryRun run;
    auto created = DawnSharedIoProvider::create(
        {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
         .proc_table_override_for_testing = proc_table,
         .fault = DawnSharedIoProvider::Fault::RejectBeforeSubmit,
         .fault_slot = 1,
         .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer});
    if (!created.provider) {
        run.error = "provider_unavailable";
        return run;
    }
    auto& provider = *created.provider;
    auto program = DawnSharedIoWavenetProgram::create(provider, fixture.spec());
    SharedIoComputePlan plan;
    const auto bytes = fixture.spec().block_size * sizeof(float);
    if (!program ||
        !plan.prepare(provider,
                      {.slots = 2,
                       .input_bytes_per_slot = bytes,
                       .output_bytes_per_slot = bytes,
                       .storage_kind = SharedIoArenaProvider::StorageKind::ImportedHostPointer},
                      std::move(program))) {
        run.error = "plan_prepare_failed";
        return run;
    }
    // Returns the slot the block was leased on, and whether the provider took it.
    struct Admitted {
        bool leased = false;
        bool accepted = false;
        std::uint32_t slot = 0;
    };
    const auto submit = [&](std::uint64_t block) {
        Admitted admitted;
        auto write = plan.acquire_input(block, 0);
        if (!write || write->bytes.size() != bytes)
            return admitted;
        admitted.leased = true;
        admitted.slot = write->token.slot;
        const std::array<float, 2> input{static_cast<float>(block),
                                         static_cast<float>(block) + 0.25f};
        std::copy(input.begin(), input.end(), reinterpret_cast<float*>(write->bytes.data()));
        const SharedIoComputePlan::SubmitToken token{write->token, 0};
        admitted.accepted = plan.submit(token);
        if (!admitted.accepted)
            (void)plan.cancel(token);
        return admitted;
    };
    const auto collect = [&](std::uint64_t block) {
        std::optional<SharedIoComputePlan::Completion> completion;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!completion && Clock::now() < deadline) {
            plan.drain(now_ns());
            completion = plan.pop_completion();
            if (!completion)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!completion || completion->token.slot.stream_sequence != block)
            return false;
        auto output = plan.acquire_output(*completion);
        if (!output || output->bytes.size() != bytes)
            return false;
        const float previous = block == 0 ? 0.0f : static_cast<float>(block - 1) + 0.25f;
        const std::array<float, 2> expected{std::tanh(0.5f * previous),
                                            std::tanh(0.5f * static_cast<float>(block))};
        if (!compare_block({reinterpret_cast<const float*>(output->bytes.data()), 2}, expected))
            ++run.mismatches;
        ++run.delivered;
        return plan.release_output({output->token});
    };
    constexpr std::uint64_t kRefusedBlock = 11;
    for (std::uint64_t block = 0; block < 16; ++block) {
        const auto admitted = submit(block);
        if (block == kRefusedBlock) {
            run.refused = admitted.leased && !admitted.accepted && admitted.slot == 1;
            if (!collect(block - 1)) {
                run.error = "collect_failed_at_" + std::to_string(block - 1);
                break;
            }
            const auto retry = submit(block);
            run.retried = retry.accepted && retry.slot == 0;
            if (!run.retried) {
                run.error = "retry_failed";
                break;
            }
        } else if (!admitted.accepted) {
            run.error = "submit_failed_at_" + std::to_string(block);
            break;
        }
        if (block == kRefusedBlock - 1)
            continue; // left in flight on slot 0
        if (!collect(block)) {
            run.error = "collect_failed_at_" + std::to_string(block);
            break;
        }
    }
    if (!plan.release() && run.error.empty())
        run.error = "release_failed";
    return run;
}

void run_node_faults(Result& result, const Fixture& fixture, const void* proc_table) {
    using ga_reason = pulp::gpu_audio::GpuAudioRecoveryReason;
    using ga_state = pulp::gpu_audio::GpuAudioProviderState;
    const auto loss = run_node_fault(
        fixture, proc_table, DawnSharedIoProvider::Fault::ForceLossAfterCompletionRegistration);
    result.node_loss_reason = loss.reason_text;
    result.node_loss_sequence = loss.sequence;
    result.node_loss_blocks = loss.blocks;
    result.node_loss_receipt = loss.ran && loss.reason == ga_reason::ProviderLost &&
                               loss.sequence == 0 && loss.state == ga_state::Unavailable &&
                               loss.status_valid;
    const auto hang =
        run_node_fault(fixture, proc_table, DawnSharedIoProvider::Fault::DelayCompletion);
    result.node_hang_reason = hang.reason_text;
    result.node_hang_sequence = hang.sequence;
    result.node_hang_blocks = hang.blocks;
    const auto reacquire_loss =
        run_node_fault(fixture, proc_table,
                       DawnSharedIoProvider::Fault::ForceLossAfterCompletionRegistration, true);
    result.reacquire_loss_reason = reacquire_loss.reason_text;
    result.reacquire_loss_reprepare_ns = reacquire_loss.reprepare_ns;
    result.reacquire_loss_resume_ns = reacquire_loss.resume_ns;
    result.reacquire_loss_receipt = reacquire_loss.reason == ga_reason::ProviderLost &&
                                    reacquire_loss.resumed_with_parity &&
                                    reacquire_loss.epoch == 2 && reacquire_loss.status_valid;
    const auto reacquire_hang =
        run_node_fault(fixture, proc_table, DawnSharedIoProvider::Fault::DelayCompletion, true);
    result.reacquire_hang_reason = reacquire_hang.reason_text;
    result.reacquire_hang_reprepare_ns = reacquire_hang.reprepare_ns;
    result.reacquire_hang_resume_ns = reacquire_hang.resume_ns;
    result.reacquire_hang_receipt = reacquire_hang.reason == ga_reason::ProviderUnresponsive &&
                                    reacquire_hang.resumed_with_parity &&
                                    reacquire_hang.epoch == 2 && reacquire_hang.status_valid;
    result.node_hang_receipt = hang.ran && hang.reason == ga_reason::ProviderUnresponsive &&
                               hang.sequence == 0 && hang.state == ga_state::Unavailable &&
                               hang.status_valid;
    const auto neural_loss = run_neural_node_fault(
        fixture, proc_table, DawnSharedIoProvider::Fault::ForceLossBeforeSubmit);
    result.neural_loss_reason = neural_loss.reason_text;
    result.neural_loss_sequence = neural_loss.sequence;
    result.neural_loss_blocks = neural_loss.blocks;
    result.neural_loss_receipt = neural_loss.ran && neural_loss.reason == ga_reason::ProviderLost &&
                                 neural_loss.sequence == 0 &&
                                 neural_loss.state == ga_state::Unavailable &&
                                 neural_loss.status_valid;
    const auto neural_hang =
        run_neural_node_fault(fixture, proc_table, DawnSharedIoProvider::Fault::DelayCompletion);
    result.neural_hang_reason = neural_hang.reason_text;
    result.neural_hang_sequence = neural_hang.sequence;
    result.neural_hang_blocks = neural_hang.blocks;
    result.neural_hang_receipt =
        neural_hang.ran && neural_hang.reason == ga_reason::ProviderUnresponsive &&
        neural_hang.sequence == 0 && neural_hang.state == ga_state::Unavailable &&
        neural_hang.status_valid;

    using Fault = DawnSharedIoProvider::Fault;
    constexpr auto kCallbacks = kAdmissionCallbacks;
    const auto clean = [](const AdmissionRun& run) {
        return run.sequence_agrees() && run.statuses_valid && run.released && run.error.empty();
    };
    const auto latched = [&](const AdmissionRun& run, ga_reason reason, std::uint64_t sequence,
                             ga_state state) {
        return clean(run) && run.reason == reason && run.sequence == sequence &&
               run.latched_after == sequence && run.state == state;
    };
    // A refusal latches ProviderFailure at the refused block in the pump that
    // saw it, so the latch names the first refused sequence.
    const auto refused_at = [&](const AdmissionRun& run, std::uint64_t sequence) {
        return run.refused_submits >= 1 && run.first_refused == sequence &&
               latched(run, ga_reason::ProviderFailure, sequence, ga_state::Degraded);
    };
    // A late block is a miss, not a fault, so a loaded host may lose a few
    // deliveries; none may mismatch.
    constexpr std::uint64_t kLateAllowance = 6;
    auto& healthy = result.admission_healthy;
    healthy = run_admission(fixture, proc_table, {});
    result.admission_healthy_receipt = clean(healthy) && healthy.reason == ga_reason::None &&
                                       healthy.gpu_blocks + kLateAllowance >= kCallbacks - 1 &&
                                       healthy.mismatches == 0;
    // Loss is forced on the first submission, so it latches at 0 with no GPU
    // block before it.
    auto& lost = result.admission_loss;
    lost = run_admission(fixture, proc_table, {.fault = Fault::ForceLossBeforeSubmit});
    result.admission_loss_receipt =
        latched(lost, ga_reason::ProviderLost, 0, ga_state::Unavailable) && lost.gpu_blocks == 0;
    // A refusal is a provider failure, not a loss.
    auto& reject = result.admission_reject;
    reject = run_admission(fixture, proc_table, {.fault = Fault::RejectBeforeSubmit});
    result.admission_reject_receipt = refused_at(reject, 0) && reject.gpu_blocks == 0;
    // With one block in flight the node takes its next block only after the
    // last one's slot is released, so a refusing slot 1 is never reached: the
    // run must refuse nothing and latch nothing. A stall seen in this
    // configuration is therefore never a refusal.
    auto& unreachable = result.admission_reject_unreachable_slot;
    unreachable =
        run_admission(fixture, proc_table, {.fault = Fault::RejectBeforeSubmit, .fault_slot = 1});
    result.admission_reject_unreachable_slot_receipt =
        clean(unreachable) && unreachable.refused_submits == 0 &&
        unreachable.reason == ga_reason::None &&
        unreachable.gpu_blocks + kLateAllowance >= kCallbacks - 1 && unreachable.mismatches == 0;
    // Withholding the pumps after callback 10 alone latches nothing, so the
    // mid-stream run's latch belongs to the refusal.
    const AdmissionScenario midstream{.fault = Fault::RejectBeforeSubmit,
                                      .fault_slot = 1,
                                      .lead = 2,
                                      .inflight = 2,
                                      .skip_pump_at = 10};
    auto control_scenario = midstream;
    control_scenario.fault = Fault::None;
    auto& control = result.admission_skip_control;
    control = run_admission(fixture, proc_table, control_scenario);
    result.admission_skip_control_receipt = clean(control) && control.reason == ga_reason::None &&
                                            control.gpu_blocks + kLateAllowance >= kCallbacks - 2 &&
                                            control.mismatches == 0;
    // Block 11 is the first admission on slot 1. Blocks 0..9 deliver on the
    // GPU at callbacks 2..11 and match the CPU reference. Block 10 was accepted
    // before the latch but is not delivered after it: the node fails closed,
    // so no callback after 11 delivers GPU output.
    auto& refused = result.admission_reject_midstream;
    refused = run_admission(fixture, proc_table, midstream);
    const auto last_gpu = refused.trace.find_last_of('G');
    result.admission_reject_midstream_receipt =
        refused_at(refused, 11) && refused.gpu_blocks + kLateAllowance >= 10 &&
        last_gpu != std::string::npos && last_gpu <= 11 && refused.mismatches == 0;

    result.refusal_retry = run_refusal_retry(fixture, proc_table);
    result.refusal_retry_receipt = result.refusal_retry.refused && result.refusal_retry.retried &&
                                   result.refusal_retry.delivered == 16 &&
                                   result.refusal_retry.mismatches == 0 &&
                                   result.refusal_retry.error.empty();
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
    // The first block is deliberately forced late; the remaining three are
    // the GPU-delivered population and must all match the independent oracle.
    result.cpu_parity = true;

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
    // No independent native-runtime identity source exists at the Dawn
    // boundary yet, so an otherwise authenticated provider is unavailable for
    // qualification: a typed skip, not a failure every physical host repeats.
    if (pulp::test::physical_qualification_outcome(
            true, pulp::gpu_audio::detail::gate_shared_io_provider_identity(identity)) ==
        pulp::test::PhysicalQualificationOutcome::Unavailable) {
        result.unavailable = true;
        result.reason = std::string(pulp::test::kNativeRuntimeIdentityUnavailable);
        emit(result, "unavailable");
        return result;
    }
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

    // This one-layer fixture has one causal pad of one sample. The provider
    // keeps the stream state in column rings sized to a power of two holding
    // the pad plus one block: the layer-input ring is 4 columns (16 bytes) and
    // the final activation ring, which carries no history, 2 columns (8 bytes).
    // Preparation zeroes each ring once. In the target-size histogram the 16-byte
    // bucket also holds the scale uniform and the four-word device stream state.
    const auto prepared_transfer = transfer_counter.snapshot();
    constexpr std::size_t expected_history_bytes = sizeof(float);
    result.history_transfer_contract =
        resident_history_bytes == expected_history_bytes &&
        prepared_transfer.queue_write_buffer_target_size_histogram[16] == 3 &&
        prepared_transfer.queue_write_buffer_target_size_histogram[8] == 1 &&
        prepared_transfer.queue_write_buffer_target_size_histogram[expected_history_bytes] == 0;

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
                result.cpu_parity &&
                compare_block({reinterpret_cast<const float*>(output->bytes.data()), 2}, expected);
            result.cpu_parity = result.cpu_parity && plan.release_output({output->token});
            ++result.gpu_delivered;
        }
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
            return receipt.terminal && receipt.submit_ns >= 0 && receipt.submit_ns != -1 &&
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

    // The rings keep history in place and the stream position advances on the
    // device, so no submission copies state or writes from the host.
    const auto transfer = transfer_counter.snapshot();
    result.resident_state_copies = transfer.copy_buffer_to_buffer_calls;
    result.history_transfer_contract = result.history_transfer_contract && result.submissions > 0 &&
                                       transfer.copy_buffer_to_buffer_calls == 0 &&
                                       transfer.copy_buffer_to_buffer_bytes == 0;
    result.zero_audio_slot_transfers =
        transfer.queue_write_buffer_calls == 0 && transfer.buffer_map_async_calls == 0 &&
        transfer.buffer_get_mapped_range_calls == 0 && transfer.buffer_unmap_calls == 0 &&
        result.history_transfer_contract;
    result.retired = result.retired && plan.release();
    run_device_loss_negative(result, fixture, transfer_counter.deferred_proc_table());
    run_node_faults(result, fixture, transfer_counter.deferred_proc_table());
    if (result.device_loss_negative)
        result.reason = result.retired ? "authenticated_one_stream_pass_cpu_fallback_shadow_only"
                                       : "release_failed";
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
        result.immutable_provenance && result.device_loss_negative && result.device_loss_receipt &&
        result.node_loss_receipt && result.node_hang_receipt && result.neural_loss_receipt &&
        result.neural_hang_receipt && result.admission_healthy_receipt &&
        result.admission_loss_receipt && result.admission_reject_receipt &&
        result.admission_reject_unreachable_slot_receipt && result.admission_skip_control_receipt &&
        result.admission_reject_midstream_receipt && result.refusal_retry_receipt &&
        result.reacquire_loss_receipt && result.reacquire_hang_receipt;
    emit(result, passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
