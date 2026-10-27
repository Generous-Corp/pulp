#include <catch2/catch_test_macros.hpp>

#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_program.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include "detail/gpu_audio_transport_trial_observer.hpp"
#include "detail/realtime_gpu_audio_path.hpp"
#include "detail/realtime_gpu_health_mirror.hpp"
#include "detail/shared_io_trace.hpp"
#include "detail/worker_pipeline_hosting.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"
#include "support/thread_progress.hpp"

#include <algorithm>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include <array>
#include <chrono>
#include <initializer_list>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <os/object.h>
#include <os/workgroup.h>
#endif
#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace pulp::gpu_audio;
using pulp::audio::BufferView;

namespace {

GpuAudioProgramDescriptor valid_shared_program() {
    return {.kind = GpuAudioProgramKind::Neural,
            .path = GpuAudioExecutionPath::SharedMemory,
            .provider = GpuAudioProvider::Dawn,
            .miss_policy = MissPolicy::CpuFallback,
            .channels = 2,
            .block_size = 32,
            .sample_rate = 48000,
            .algorithmic_lead_blocks = 2,
            .pipeline_depth = 4,
            .provider_slots = 4,
            .provider_owned_resources = true,
            .cpu_fallback_prepared = true};
}

} // namespace

TEST_CASE("GpuAudioProgramDescriptor validates a prepared shared program", "[gpu_audio][program]") {
    const auto program = valid_shared_program();
    const auto validation = validate_gpu_audio_program(program);
    CHECK(validation.accepted());
    CHECK(program.provider == GpuAudioProvider::Dawn);
    CHECK(program.provider_owned_resources);
}

TEST_CASE("GpuAudioStatus preserves provider, delivery, and fallback meanings",
          "[gpu_audio][status]") {
    GpuAudioStatus status;
    status.selected_engine = GpuAudioEngine::Gpu;
    status.provider_state = GpuAudioProviderState::Ready;
    status.provider = GpuAudioProvider::Dawn;
    status.produced_blocks = 17;
    status.missed_blocks = 3;
    status.fallback_blocks = 2;

    CHECK(status.schema_version == GpuAudioStatus::kSchemaVersion);
    CHECK(status.produced_blocks == 17);
    CHECK(status.missed_blocks == 3);
    CHECK(status.fallback_blocks == 2);
    CHECK(status.missed_blocks != status.fallback_blocks);
}

TEST_CASE("GpuAudioStatus v3 fields default to unreported", "[gpu_audio][status]") {
    const GpuAudioStatus status;
    CHECK(GpuAudioStatus::kSchemaVersion == 4);
    CHECK(status.schema_version == 4);
    CHECK(status.provider == GpuAudioProvider::Unknown);
    CHECK(status.fallback_policy == MissPolicy::Silence);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(status.recovery_sequence == UINT64_MAX);
    CHECK(status.epoch == 0);
    CHECK_FALSE(status.delivery_epoch_active);
    for (const auto count : status.terminal_counts)
        CHECK(count == 0);
    CHECK(status.completion_mode == GpuAudioCompletionMode::Unknown);
    CHECK_FALSE(status.timestamps_available);
    CHECK(status.last_delivery_ns == 0);
    CHECK_FALSE(status.gpu_kernel_available);
    CHECK(status.shadow_mode == GpuAudioShadowMode::None);
    CHECK(status.shadow_inline_cost_pct == 0.0f);
    CHECK(status.shadow_delivered_blocks == 0);
    CHECK(status.shadow_late_blocks == 0);
    CHECK(status.shadow_double_miss_blocks == 0);
    CHECK_FALSE(status.shadow_model_differs);
}

TEST_CASE("GpuAudioStatus v2 positional prefix still aggregate-initializes",
          "[gpu_audio][status]") {
    // Every v2 field, positionally, as an installed v2 consumer would write it.
    const GpuAudioStatus status{2,
                                GpuAudioEngine::Gpu,
                                GpuAudioEngine::Cpu,
                                GpuAudioProviderState::Degraded,
                                GpuAudioProvider::Dawn,
                                MissPolicy::CpuFallback,
                                48000,
                                64,
                                128,
                                2,
                                true,
                                true,
                                10,
                                3,
                                2,
                                1,
                                5,
                                12.5,
                                11.0,
                                GpuAudioRecoveryReason::ProviderLost,
                                42};
    CHECK(status.schema_version == 2);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(status.provider == GpuAudioProvider::Dawn);
    CHECK(status.fallback_policy == MissPolicy::CpuFallback);
    CHECK(status.block_size == 64);
    CHECK(status.avg_block_us == 11.0);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(status.recovery_sequence == 42);
    CHECK(status.epoch == 0);
    CHECK(status.completion_mode == GpuAudioCompletionMode::Unknown);
    CHECK_FALSE(status.gpu_kernel_available);

    // The descriptor's pre-append fields, positionally, as installed callers write them.
    const GpuAudioNodeDescriptor descriptor{"probe", 2, 2, 64, 48000, 3, MissPolicy::CpuFallback,
                                            true};
    CHECK(descriptor.latency_blocks == 3);
    CHECK(descriptor.supports_cpu_fallback);
    CHECK(descriptor.worker_pipeline_blocks == 0);
}

namespace {

GpuAudioStatus ready_gpu_status() {
    GpuAudioStatus status;
    status.requested_engine = GpuAudioEngine::Gpu;
    status.selected_engine = GpuAudioEngine::Gpu;
    status.provider_state = GpuAudioProviderState::Ready;
    status.provider = GpuAudioProvider::Dawn;
    status.fallback_policy = MissPolicy::CpuFallback;
    status.fallback_available = true;
    status.sample_rate = 48000;
    status.block_size = 64;
    status.produced_blocks = 10;
    status.fallback_blocks = 2;
    status.terminal_counts[1] = 10; // GpuDelivered
    status.terminal_counts[2] = 2;  // CpuFallback
    status.epoch = 1;
    status.delivery_epoch_active = true;
    status.gpu_kernel_available = true;
    return status;
}

GpuAudioStatusError status_error(const GpuAudioStatus& status) {
    return validate_gpu_audio_status(status).error;
}

} // namespace

TEST_CASE("validate_gpu_audio_status accepts coherent v3 and v4 states", "[gpu_audio][status]") {
    STATIC_REQUIRE(validate_gpu_audio_status(GpuAudioStatus{}).accepted());
    CHECK(validate_gpu_audio_status(ready_gpu_status()).accepted());

    auto cpu_family = ready_gpu_status();
    cpu_family.selected_engine = GpuAudioEngine::Cpu;
    cpu_family.provider = GpuAudioProvider::Cpu;
    cpu_family.gpu_kernel_available = false;
    cpu_family.epoch = 0;
    cpu_family.delivery_epoch_active = false;
    std::fill(std::begin(cpu_family.terminal_counts), std::end(cpu_family.terminal_counts), 0u);
    CHECK(validate_gpu_audio_status(cpu_family).accepted());

    auto lost = ready_gpu_status();
    lost.provider_state = GpuAudioProviderState::Unavailable;
    lost.selected_engine = GpuAudioEngine::Unknown;
    lost.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
    lost.recovery_sequence = 7;
    CHECK(validate_gpu_audio_status(lost).accepted());
    lost.recovery_reason = GpuAudioRecoveryReason::ProviderUnresponsive;
    CHECK(validate_gpu_audio_status(lost).accepted());

    auto sustained = ready_gpu_status();
    sustained.provider_state = GpuAudioProviderState::Degraded;
    sustained.selected_engine = GpuAudioEngine::Cpu;
    sustained.recovery_reason = GpuAudioRecoveryReason::SustainedDeadlineMisses;
    sustained.recovery_sequence = 9;
    CHECK(validate_gpu_audio_status(sustained).accepted());

    auto shadowed = ready_gpu_status();
    shadowed.shadow_mode = GpuAudioShadowMode::Worker;
    shadowed.shadow_delivered_blocks = 3;
    shadowed.shadow_late_blocks = 1;
    shadowed.shadow_double_miss_blocks = 1;
    shadowed.shadow_inline_cost_pct = 4.5f;
    CHECK(validate_gpu_audio_status(shadowed).accepted());

    // v4: a successful re-acquire, delivering again, measured and counted.
    auto reacquired = ready_gpu_status();
    reacquired.recovery_reason = GpuAudioRecoveryReason::ProviderReacquired;
    reacquired.recovery_sequence = 40;
    reacquired.epoch = 2;
    reacquired.reacquire_attempts = 2;
    reacquired.reacquire_successes = 1;
    reacquired.last_reacquire_to_gpu_ns = 51'000'000;
    CHECK(validate_gpu_audio_status(reacquired).accepted());
    reacquired.provider_state = GpuAudioProviderState::Degraded;
    reacquired.selected_engine = GpuAudioEngine::Cpu;
    CHECK(validate_gpu_audio_status(reacquired).accepted());
    // Failed attempts alone are coherent.
    auto failing = ready_gpu_status();
    failing.reacquire_attempts = 3;
    CHECK(validate_gpu_audio_status(failing).accepted());

    // Every cause that fences delivery leaves the provider Degraded, or
    // Unavailable when the device was lost after it latched, on the CPU
    // fallback; the fenced stamped node's delivery epoch is inactive.
    for (const auto reason :
         {GpuAudioRecoveryReason::SequenceGap, GpuAudioRecoveryReason::InputSaturated,
          GpuAudioRecoveryReason::ProviderFailure, GpuAudioRecoveryReason::InvalidCallback,
          GpuAudioRecoveryReason::OfflineFence}) {
        CAPTURE(reason);
        auto fenced = ready_gpu_status();
        fenced.provider_state = GpuAudioProviderState::Degraded;
        fenced.selected_engine = GpuAudioEngine::Cpu;
        fenced.recovery_reason = reason;
        fenced.recovery_sequence = 12;
        fenced.delivery_epoch_active = false;
        fenced.gpu_kernel_available = reason != GpuAudioRecoveryReason::OfflineFence;
        CHECK(validate_gpu_audio_status(fenced).accepted());
        fenced.provider_state = GpuAudioProviderState::Unavailable;
        CHECK(validate_gpu_audio_status(fenced).accepted());
    }
    // A stamped node between a re-acquire request and its adoption: no cause
    // latched, the new epoch named, delivery not yet active.
    auto adopting = ready_gpu_status();
    adopting.provider_state = GpuAudioProviderState::Degraded;
    adopting.selected_engine = GpuAudioEngine::Cpu;
    adopting.epoch = 3;
    adopting.delivery_epoch_active = false;
    adopting.reacquire_attempts = adopting.reacquire_successes = 1;
    CHECK(validate_gpu_audio_status(adopting).accepted());
}

TEST_CASE("validate_gpu_audio_status rejects each planted v3 violation", "[gpu_audio][status]") {
    using Error = GpuAudioStatusError;

    auto status = ready_gpu_status();
    status.schema_version = 2;
    CHECK(status_error(status) == Error::SchemaVersionMismatch);

    status = ready_gpu_status();
    status.shadow_mode = static_cast<GpuAudioShadowMode>(7);
    CHECK(status_error(status) == Error::InvalidEnumValue);
    status = ready_gpu_status();
    status.recovery_reason = static_cast<GpuAudioRecoveryReason>(10);
    status.recovery_sequence = 1;
    CHECK(status_error(status) == Error::InvalidEnumValue);

    // recovery_sequence == UINT64_MAX iff no recovery reason.
    status = ready_gpu_status();
    status.recovery_reason = GpuAudioRecoveryReason::ProviderFailure;
    CHECK(status_error(status) == Error::RecoverySequenceMismatch);
    status = ready_gpu_status();
    status.recovery_sequence = 3;
    CHECK(status_error(status) == Error::RecoverySequenceMismatch);

    status = GpuAudioStatus{};
    status.sample_rate = 48000;
    CHECK(status_error(status) == Error::UninitializedStateHasPreparation);
    status = GpuAudioStatus{};
    status.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
    status.recovery_sequence = 1;
    CHECK(status_error(status) == Error::UninitializedStateHasRecovery);

    status = ready_gpu_status();
    status.block_size = 0;
    CHECK(status_error(status) == Error::PreparedShapeMissing);

    status = ready_gpu_status();
    status.selected_engine = GpuAudioEngine::Unknown;
    CHECK(status_error(status) == Error::ReadyWithoutEngine);

    status = ready_gpu_status();
    status.provider = GpuAudioProvider::Cpu;
    status.gpu_kernel_available = false;
    status.epoch = 0;
    CHECK(status_error(status) == Error::GpuEngineWithoutProvider);

    // Degraded or Unavailable never selects the GPU engine.
    status = ready_gpu_status();
    status.provider_state = GpuAudioProviderState::Degraded;
    CHECK(status_error(status) == Error::DegradedOrUnavailableOnGpu);
    status.provider_state = GpuAudioProviderState::Unavailable;
    CHECK(status_error(status) == Error::DegradedOrUnavailableOnGpu);

    // ProviderLost / ProviderUnresponsive imply Unavailable.
    status = ready_gpu_status();
    status.provider_state = GpuAudioProviderState::Degraded;
    status.selected_engine = GpuAudioEngine::Unknown;
    status.recovery_reason = GpuAudioRecoveryReason::ProviderUnresponsive;
    status.recovery_sequence = 4;
    CHECK(status_error(status) == Error::LostProviderNotUnavailable);
    status.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
    CHECK(status_error(status) == Error::LostProviderNotUnavailable);

    // SustainedDeadlineMisses implies Degraded.
    status = ready_gpu_status();
    status.provider_state = GpuAudioProviderState::Unavailable;
    status.selected_engine = GpuAudioEngine::Unknown;
    status.recovery_reason = GpuAudioRecoveryReason::SustainedDeadlineMisses;
    status.recovery_sequence = 4;
    CHECK(status_error(status) == Error::SustainedMissesNotDegraded);

    // A CPU provider has no GPU kernel and no GPU delivery epoch.
    status = ready_gpu_status();
    status.selected_engine = GpuAudioEngine::Cpu;
    status.provider = GpuAudioProvider::Cpu;
    status.epoch = 0;
    CHECK(status_error(status) == Error::CpuProviderWithGpuState);
    status.gpu_kernel_available = false;
    status.epoch = 2;
    CHECK(status_error(status) == Error::CpuProviderWithGpuState);

    // No shadow means no shadow accounting.
    status = ready_gpu_status();
    status.shadow_late_blocks = 1;
    CHECK(status_error(status) == Error::ShadowCountsWithoutShadow);
    status = ready_gpu_status();
    status.shadow_inline_cost_pct = 1.0f;
    CHECK(status_error(status) == Error::ShadowCountsWithoutShadow);

    // Terminal counts stay within the delivery counters and do not overflow.
    status = ready_gpu_status();
    status.terminal_counts[1] = status.produced_blocks + 1;
    CHECK(status_error(status) == Error::TerminalCountsInconsistent);
    status = ready_gpu_status();
    status.terminal_counts[2] = status.fallback_blocks + 1;
    CHECK(status_error(status) == Error::TerminalCountsInconsistent);
    status = ready_gpu_status();
    status.terminal_counts[3] = UINT64_MAX;
    CHECK(status_error(status) == Error::TerminalCountsInconsistent);

    status = ready_gpu_status();
    status.fallback_available = false;
    CHECK(status_error(status) == Error::FallbackUnavailable);

    // A v3 snapshot is not a v4 one.
    status = ready_gpu_status();
    status.schema_version = 3;
    CHECK(status_error(status) == Error::SchemaVersionMismatch);

    // Re-acquire accounting.
    status = ready_gpu_status();
    status.reacquire_attempts = 1;
    status.reacquire_successes = 2;
    CHECK(status_error(status) == Error::ReacquireSuccessesExceedAttempts);
    status = ready_gpu_status();
    status.recovery_reason = GpuAudioRecoveryReason::ProviderReacquired;
    status.recovery_sequence = 5;
    status.reacquire_attempts = 1;
    CHECK(status_error(status) == Error::ReacquiredWithoutSuccess);
    status.reacquire_successes = 1;
    status.provider_state = GpuAudioProviderState::Unavailable;
    status.selected_engine = GpuAudioEngine::Unknown;
    CHECK(status_error(status) == Error::ReacquiredNotDelivering);
    status = ready_gpu_status();
    status.reacquire_attempts = 1;
    status.last_reacquire_to_gpu_ns = 10;
    CHECK(status_error(status) == Error::ReacquireTimingWithoutSuccess);

    // The GPU engine needs a prepared kernel; the same status on the CPU
    // engine, with the kernel gone, is fine.
    status = ready_gpu_status();
    status.gpu_kernel_available = false;
    CHECK(status_error(status) == Error::GpuEngineWithoutKernel);
    status.selected_engine = GpuAudioEngine::Cpu;
    status.terminal_counts[1] = 0;
    CHECK(status_error(status) == Error::None);

    // Precedence: a status breaking a re-acquire rule and the kernel rule
    // reports the re-acquire error; the kernel rule is checked last.
    status = ready_gpu_status();
    status.gpu_kernel_available = false;
    status.reacquire_attempts = 1;
    status.reacquire_successes = 2;
    CHECK(status_error(status) == Error::ReacquireSuccessesExceedAttempts);
    status.reacquire_successes = 1;
    CHECK(status_error(status) == Error::GpuEngineWithoutKernel);

    // A cause that fences delivery never leaves the provider Ready, on any
    // engine.
    for (const auto reason :
         {GpuAudioRecoveryReason::SequenceGap, GpuAudioRecoveryReason::InputSaturated,
          GpuAudioRecoveryReason::ProviderFailure, GpuAudioRecoveryReason::InvalidCallback,
          GpuAudioRecoveryReason::OfflineFence}) {
        CAPTURE(reason);
        status = ready_gpu_status();
        status.recovery_reason = reason;
        status.recovery_sequence = 12;
        CHECK(status_error(status) == Error::FencingCauseReportedReady);
        status.selected_engine = GpuAudioEngine::Cpu;
        CHECK(status_error(status) == Error::FencingCauseReportedReady);
    }

    // A stamped node whose delivery epoch is inactive is fenced, so not Ready.
    status = ready_gpu_status();
    status.delivery_epoch_active = false;
    CHECK(status_error(status) == Error::ReadyWithoutDeliveryEpoch);
    status.selected_engine = GpuAudioEngine::Cpu;
    CHECK(status_error(status) == Error::ReadyWithoutDeliveryEpoch);
    // Without an epoch the node is not stamped: a CPU provider is Ready.
    status.provider = GpuAudioProvider::Cpu;
    status.gpu_kernel_available = false;
    status.epoch = 0;
    std::fill(std::begin(status.terminal_counts), std::end(status.terminal_counts), 0u);
    CHECK(status_error(status) == Error::None);
}

TEST_CASE("GPU audio contract enums append without renumbering", "[gpu_audio][status]") {
    CHECK(static_cast<int>(GpuAudioStatusError::GpuEngineWithoutKernel) == 20);
    CHECK(static_cast<int>(GpuAudioStatusError::FencingCauseReportedReady) == 21);
    CHECK(static_cast<int>(GpuAudioStatusError::ReadyWithoutDeliveryEpoch) == 22);
    CHECK(static_cast<int>(GpuAudioRecoveryReason::InvalidCallback) == 6);
    CHECK(static_cast<int>(GpuAudioRecoveryReason::ProviderUnresponsive) == 7);
    CHECK(static_cast<int>(GpuAudioRecoveryReason::ProviderReacquired) == 8);
    CHECK(static_cast<int>(GpuAudioRecoveryReason::SustainedDeadlineMisses) == 9);
    CHECK(static_cast<int>(GpuAudioTerminalDisposition::Cancelled) == 7);
    CHECK(static_cast<std::uint32_t>(GpuAudioTerminalDisposition::kCount) ==
          GpuAudioStatus::kTerminalDispositionCount);
    CHECK(static_cast<int>(GpuAudioShadowMode::Worker) == 2);
    CHECK(static_cast<int>(GpuAudioProvider::Metal) == 2);
    CHECK(static_cast<int>(GpuAudioProvider::Cpu) == 3);
    const GpuAudioCapabilityReport report;
    CHECK_FALSE(report.gpu_kernel_available);
    CHECK(report.recovery_policy == GpuAudioRecoveryPolicy::Manual);
    const detail::RealtimeGpuNodePath path;
    CHECK(path.health == nullptr);
    CHECK(path.reprepare == nullptr);
    const GpuAudioNodeDescriptor descriptor;
    CHECK(descriptor.worker_pipeline_blocks == 0);
}

TEST_CASE("GpuAudioNode appended hooks have inert defaults", "[gpu_audio][status]") {
    struct MinimalNode final : GpuAudioNode {
        GpuAudioNodeDescriptor descriptor() const override {
            return {};
        }
        bool prepare() override {
            return true;
        }
        void process_block(const pulp::audio::BufferView<const float>&,
                           pulp::audio::BufferView<float>&, uint32_t) override {}
    };
    MinimalNode node;
    GpuAudioNode& base = node;
    const auto status = base.status_snapshot();
    CHECK(validate_gpu_audio_status(status).accepted());
    CHECK(status.schema_version == GpuAudioStatus::kSchemaVersion);
    CHECK(status.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(base.release_bounded(0));
}

TEST_CASE("GpuAudioProgramDescriptor rejects incomplete shared preparation",
          "[gpu_audio][program]") {
    auto program = valid_shared_program();

    program.algorithmic_lead_blocks = 0;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::MissingAlgorithmicLead);

    program.algorithmic_lead_blocks = 2;
    program.pipeline_depth = 2;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::InsufficientPipelineDepth);

    program.pipeline_depth = 4;
    program.provider = GpuAudioProvider::Unknown;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::MissingProviderIdentity);
}

TEST_CASE("GpuAudioProgramDescriptor rejects unsafe provider and fallback claims",
          "[gpu_audio][program]") {
    auto program = valid_shared_program();

    program.provider_owned_resources = false;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::ProviderResourcesNotOwned);

    program = valid_shared_program();
    program.miss_policy = MissPolicy::CpuFallback;
    program.cpu_fallback_prepared = false;
    CHECK(validate_gpu_audio_program(program).error ==
          GpuAudioProgramError::CpuFallbackNotPrepared);

    program = valid_shared_program();
    program.path = GpuAudioExecutionPath::Cpu;
    program.provider = GpuAudioProvider::Unknown;
    program.provider_owned_resources = false;
    program.algorithmic_lead_blocks = 0;
    program.pipeline_depth = 0;
    program.provider_slots = 0;
    program.miss_policy = MissPolicy::Silence;
    CHECK(validate_gpu_audio_program(program).accepted());
}

TEST_CASE("GpuAudioProgramDescriptor binds only to an authenticated capability",
          "[gpu_audio][program]") {
    const auto program = valid_shared_program();
    GpuAudioCapabilityReport capability;
    capability.path = GpuAudioExecutionPath::SharedMemory;
    capability.provider = GpuAudioProvider::Dawn;
    capability.eligibility = GpuAudioEligibility::Eligible;
    capability.fallback_policy = MissPolicy::CpuFallback;
    capability.prepared_lead_blocks = 2;
    capability.prepared = true;
    capability.fallback_available = true;

    CHECK(validate_gpu_audio_program(program, capability).accepted());

    capability.provider = GpuAudioProvider::Unknown;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityProviderMismatch);

    capability = {};
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityNotPrepared);
}

TEST_CASE("GpuAudioProgramDescriptor rejects capability drift before execution",
          "[gpu_audio][program]") {
    const auto program = valid_shared_program();
    GpuAudioCapabilityReport capability;
    capability.path = GpuAudioExecutionPath::SharedMemory;
    capability.provider = GpuAudioProvider::Dawn;
    capability.eligibility = GpuAudioEligibility::Eligible;
    capability.fallback_policy = MissPolicy::CpuFallback;
    capability.prepared_lead_blocks = 2;
    capability.prepared = true;
    capability.fallback_available = true;

    capability.path = GpuAudioExecutionPath::Staged;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityPathMismatch);

    capability.path = GpuAudioExecutionPath::SharedMemory;
    capability.prepared_lead_blocks = 4;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityLeadMismatch);

    capability.prepared_lead_blocks = 2;
    capability.fallback_available = false;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityFallbackUnavailable);

    capability.fallback_available = true;
    capability.fallback_policy = MissPolicy::Silence;
    CHECK(validate_gpu_audio_program(program, capability).error ==
          GpuAudioProgramError::CapabilityFallbackPolicyMismatch);
}

namespace pulp::gpu_audio::test_detail {

class RealtimeHookNode : public GpuAudioNode {
  public:
    RealtimeHookNode(uint32_t channels, uint32_t block, MissPolicy policy, uint32_t latency = 2)
        : channels_(channels), block_(block), policy_(policy), latency_(latency) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "realtime-hook";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = policy_;
        d.supports_cpu_fallback = policy_ == MissPolicy::CpuFallback;
        return d;
    }

    bool prepare() override {
        next_sequence = 0;
        fallback_delay_index_ = 0;
        fallback_delay_.assign(static_cast<std::size_t>(latency_) * channels_ * block_, 0.0f);
        fallback_due_.assign(static_cast<std::size_t>(channels_) * block_, 0.0f);
        return true;
    }

    bool cpu_fallback_prepared() const noexcept override {
        return policy_ == MissPolicy::CpuFallback;
    }

    void process_block(const BufferView<const float>& input, BufferView<float>& output,
                       uint32_t n) override {
        ++process_block_calls;
        output.clear();
        for (uint32_t c = 0; c < channels_ && c < output.num_channels(); ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                dst[i] = src[i] * 10.0f;
        }
    }

    void process_cpu_fallback(const BufferView<const float>& input, BufferView<float>& output,
                              uint32_t n) noexcept override {
        ++cpu_fallback_calls;
        output.clear();
        for (uint32_t c = 0; c < channels_ && c < output.num_channels(); ++c) {
            const float* src = stateful_fallback
                                   ? fallback_due_.data() + static_cast<std::size_t>(c) * block_
                                   : input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                dst[i] = stateful_fallback ? src[i] : src[i] * -1.0f;
        }
    }

    void prime_fallback(const BufferView<const float>& input, uint32_t n) noexcept override {
        ++prime_fallback_calls;
        if (!stateful_fallback || n != block_)
            return;
        auto* slot = fallback_delay_.data() +
                     static_cast<std::size_t>(fallback_delay_index_) * channels_ * block_;
        std::copy_n(slot, fallback_due_.size(), fallback_due_.data());
        for (uint32_t c = 0; c < channels_; ++c)
            std::copy_n(input.channel_ptr(c), block_, slot + static_cast<std::size_t>(c) * block_);
        fallback_delay_index_ = (fallback_delay_index_ + 1) % latency_;
    }

    // Registers through the same hook every shipped node uses; the transport
    // never names this type.
    bool bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept override {
        binding.path = path();
        binding.identity_context = this;
        binding.identity = [](const void* self) noexcept {
            const auto* node = static_cast<const RealtimeHookNode*>(self);
            detail::SharedIoProviderIdentity identity;
            identity.provider = node->identity_provider;
            identity.authenticated = node->claims_provider;
            return identity;
        };
        return true;
    }

    detail::RealtimeGpuNodePath path() noexcept {
        return {.context = this,
                .process = &RealtimeHookNode::process_realtime,
                .service = &RealtimeHookNode::service_realtime,
                .fence = &RealtimeHookNode::fence_realtime,
                .delivered = &RealtimeHookNode::delivered_realtime,
                .next_sequence = &RealtimeHookNode::get_next_sequence,
                .health = expose_health ? &RealtimeHookNode::health_realtime : nullptr,
                .reprepare = expose_reprepare ? &RealtimeHookNode::reprepare_realtime : nullptr,
                .set_completion_notifier =
                    expose_notifier ? &RealtimeHookNode::set_notifier_realtime : nullptr};
    }

    // Private test hook: latch a recovery cause as the node's worker would.
    void inject_recovery(GpuAudioRecoveryReason reason, std::uint64_t sequence) noexcept {
        health.fenced = true;
        health.delivery_epoch_active = false;
        if (health.recovery_reason == GpuAudioRecoveryReason::None) {
            health.recovery_reason = reason;
            health.recovery_sequence = sequence;
        }
    }

    std::uint64_t last_sequence = 0;
    std::uint64_t delivered_sequence = 0;
    std::uint32_t delivery_calls = 0;
    detail::SharedIoDeliveryDisposition delivery = detail::SharedIoDeliveryDisposition::None;
    std::uint8_t realtime_status = detail::kRealtimeGpuReady;
    std::uint32_t service_return_blocks = 3;
    bool fence_result = true;
    std::uint32_t realtime_calls = 0;
    std::atomic<std::uint32_t> service_calls{0};
    std::uint32_t fence_calls = 0;
    std::uint32_t process_block_calls = 0;
    std::uint32_t prime_fallback_calls = 0;
    std::uint32_t cpu_fallback_calls = 0;
    bool stateful_fallback = false;
    bool enforce_sequence = false;
    bool sequence_fenced = false;
    std::uint64_t next_sequence = 0;
    std::uint32_t stale_callback_calls = 0;
    std::uint32_t sequence_gap_calls = 0;
    std::uint32_t next_sequence_queries = 0;
    std::uint32_t invalid_callback_calls = 0;
    bool invalid_input_zero_filled = true;
    bool callback_shape_valid = true;
    const float* external_output_sample = nullptr;
    bool external_output_silent_on_rejection = true;
    bool expose_health = false;
    bool expose_reprepare = false;
    std::uint32_t reprepare_calls = 0;
    // Worker wake-latency probe (benchmarks): set by a test thread, consumed
    // by the worker's service step. Reserve capacity before the worker runs.
    std::atomic<std::uint64_t> completion_ready_ns{0};
    std::vector<std::uint64_t> wake_latencies_ns;
    // Service gate: while hold_service is set, the worker's next service step
    // parks inside the pump (bounded by the progress deadline) and raises
    // service_held, so a test can land events while the worker is busy.
    std::atomic<bool> hold_service{false};
    std::atomic<bool> service_held{false};
    // Provider side of the completion-notifier seam: what a backend callback
    // thread would call after pushing a terminal record.
    void notify_completion() noexcept {
        // Same discipline a provider uses: count the call in flight so a
        // clearing call can wait it out, then read the installed pair.
        notify_in_flight.fetch_add(1, std::memory_order_acq_rel);
        const auto notify = installed_notify.load(std::memory_order_acquire);
        if (notify != nullptr)
            notify(installed_context.load(std::memory_order_acquire));
        notify_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
    bool expose_notifier = false;
    bool notifier_supported = true;
    std::atomic<detail::SharedIoCompletionNotifyFn> installed_notify{nullptr};
    std::atomic<void*> installed_context{nullptr};
    std::atomic<std::uint32_t> notify_in_flight{0};
    std::uint32_t notifier_installs = 0;
    std::uint32_t notifier_clears = 0;
    bool claims_provider = false;
    // The provider kind this node's identity reports; it counts as a GPU
    // provider only when claims_provider also authenticates it.
    GpuAudioProvider identity_provider = GpuAudioProvider::Dawn;
    bool health_read_ok = true;
    std::uint32_t health_calls = 0;
    // Hook contract: never called from a host/UI reader thread.
    std::thread::id forbidden_health_thread{};
    std::atomic<std::uint32_t> health_calls_from_forbidden_thread{0};
    detail::RealtimeGpuHealth health = [] {
        detail::RealtimeGpuHealth initial;
        initial.epoch = 1;
        initial.delivery_epoch_active = true;
        return initial;
    }();

  private:
    static bool set_notifier_realtime(void* self,
                                      detail::SharedIoCompletionNotifier notifier) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        if (!node->notifier_supported)
            return false;
        (notifier.notify != nullptr ? node->notifier_installs : node->notifier_clears) += 1;
        if (notifier.notify == nullptr) {
            node->installed_notify.store(nullptr, std::memory_order_release);
            // The clear-time guarantee: no invocation outlives this call. Each
            // notify in flight is one semaphore post, so this waits out a few
            // instructions; it models the provider contract.
            while (node->notify_in_flight.load(std::memory_order_acquire) !=
                   0) // unbounded-wait: allow a notify is one semaphore post
                std::this_thread::yield();
            node->installed_context.store(nullptr, std::memory_order_release);
        } else {
            node->installed_context.store(notifier.context, std::memory_order_release);
            node->installed_notify.store(notifier.notify, std::memory_order_release);
        }
        return true;
    }

    // A successful re-acquire: a new, unfenced epoch whose GPU delivers.
    static bool reprepare_realtime(void* self, std::uint64_t) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->reprepare_calls;
        node->health.fenced = false;
        node->health.delivery_epoch_active = true;
        node->health.recovery_reason = GpuAudioRecoveryReason::ProviderReacquired;
        node->health.recovery_sequence = node->next_sequence;
        ++node->health.epoch;
        node->realtime_status = detail::kRealtimeGpuReady;
        return true;
    }

    static bool health_realtime(const void* self, detail::RealtimeGpuHealth& out) noexcept {
        auto* node = const_cast<RealtimeHookNode*>(static_cast<const RealtimeHookNode*>(self));
        ++node->health_calls;
        if (std::this_thread::get_id() == node->forbidden_health_thread)
            node->health_calls_from_forbidden_thread.fetch_add(1, std::memory_order_relaxed);
        if (!node->health_read_ok)
            return false;
        out = node->health;
        return true;
    }

    static std::uint8_t process_realtime(void* self, const BufferView<const float>& input,
                                         BufferView<float>& output, std::uint32_t n,
                                         std::uint64_t sequence, bool input_valid,
                                         std::uint64_t /*callback_start_ns*/) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        node->last_sequence = sequence;
        ++node->realtime_calls;
        if (node->enforce_sequence && sequence != node->next_sequence) {
            if (sequence < node->next_sequence)
                ++node->stale_callback_calls;
            else
                ++node->sequence_gap_calls;
            node->sequence_fenced = true;
        }
        node->next_sequence = std::max(node->next_sequence, sequence + 1);
        const bool shape_valid = n == node->block_ && input.num_channels() == node->channels_ &&
                                 output.num_channels() == node->channels_ &&
                                 input.num_samples() >= n && output.num_samples() >= n;
        node->callback_shape_valid &= shape_valid;
        if (!input_valid) {
            ++node->invalid_callback_calls;
            node->sequence_fenced = true;
            node->invalid_input_zero_filled &= shape_valid;
            if (shape_valid) {
                for (uint32_t c = 0; c < node->channels_; ++c)
                    for (uint32_t i = 0; i < n; ++i)
                        node->invalid_input_zero_filled &= input.channel_ptr(c)[i] == 0.0f;
            }
        }
        if (node->sequence_fenced)
            return detail::kRealtimeGpuMissed;
        if (node->realtime_status == detail::kRealtimeGpuReady) {
            output.clear();
            for (uint32_t c = 0; c < node->channels_ && c < output.num_channels(); ++c) {
                const float* src = input.channel_ptr(c);
                float* dst = output.channel_ptr(c);
                for (uint32_t i = 0; i < n; ++i)
                    dst[i] = src[i] + 1000.0f;
            }
        }
        return node->realtime_status;
    }

    static std::uint64_t get_next_sequence(void* self) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->next_sequence_queries;
        return node->next_sequence;
    }

    static void delivered_realtime(void* self, std::uint64_t sequence, std::uint8_t disposition,
                                   std::uint64_t /*callback_end_ns*/,
                                   std::uint64_t /*result_visible_ns*/) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->delivery_calls;
        node->delivered_sequence = sequence;
        node->delivery = static_cast<detail::SharedIoDeliveryDisposition>(disposition);
        if (node->delivery == detail::SharedIoDeliveryDisposition::InvalidRejected &&
            node->external_output_sample)
            node->external_output_silent_on_rejection &= *node->external_output_sample == 0.0f;
    }

    static std::uint32_t service_realtime(void* self, std::uint64_t) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->service_calls;
        if (node->hold_service.load(std::memory_order_acquire)) {
            node->service_held.store(true, std::memory_order_release);
            (void)pulp::test::wait_for_condition(
                [node] { return !node->hold_service.load(std::memory_order_acquire); });
            node->service_held.store(false, std::memory_order_release);
        }
        // Wake-latency probe: a completion "became ready" at this time; the
        // worker observes it here, in its service step.
        if (const auto ready = node->completion_ready_ns.exchange(0, std::memory_order_acq_rel);
            ready != 0 && node->wake_latencies_ns.size() < node->wake_latencies_ns.capacity()) {
            const auto now =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
            node->wake_latencies_ns.push_back(now > ready ? now - ready : 0);
        }
        return node->service_return_blocks;
    }

    static bool fence_realtime(void* self) noexcept {
        auto* node = static_cast<RealtimeHookNode*>(self);
        ++node->fence_calls;
        node->realtime_status = detail::kRealtimeGpuMissed;
        return node->fence_result;
    }

    uint32_t channels_;
    uint32_t block_;
    MissPolicy policy_;
    uint32_t latency_;
    uint32_t fallback_delay_index_ = 0;
    std::vector<float> fallback_delay_;
    std::vector<float> fallback_due_;
};

} // namespace pulp::gpu_audio::test_detail

TEST_CASE("GpuAudioTransport capability report is an honest staged snapshot",
          "[gpu_audio][transport][capability]") {
    constexpr uint32_t BS = 32;
    GpuAudioTransport transport;

    const auto inactive = transport.capability_report();
    CHECK(inactive.path == GpuAudioExecutionPath::Unavailable);
    CHECK(inactive.provider == GpuAudioProvider::Unknown);
    CHECK(inactive.eligibility == GpuAudioEligibility::Unavailable);
    CHECK_FALSE(inactive.prepared);
    CHECK(inactive.prepared_latency_blocks == 0);
    CHECK(inactive.prepared_lead_blocks == 0);
    CHECK_FALSE(inactive.fallback_available);
    CHECK_FALSE(inactive.diagnostics_available);

    test_detail::RealtimeHookNode node(1, BS, MissPolicy::CpuFallback, 3);
    REQUIRE(node.prepare());
    REQUIRE(transport.prepare(&node, {.ring_blocks = 8}));

    const auto report = transport.capability_report();
    // The test hook exercises the private seam but has no authenticated
    // provider identity, so the report must classify it as staged.
    CHECK(report.path == GpuAudioExecutionPath::Staged);
    CHECK(report.provider == GpuAudioProvider::Unknown);
    CHECK(report.eligibility == GpuAudioEligibility::Eligible);
    CHECK(report.prepared);
    CHECK(report.prepared_latency_blocks == 3);
    CHECK(report.prepared_lead_blocks == 0);
    CHECK(report.fallback_policy == MissPolicy::CpuFallback);
    CHECK(report.fallback_available);
    CHECK(report.diagnostics_available);

    transport.release();
    const auto released = transport.capability_report();
    CHECK(released.path == GpuAudioExecutionPath::Unavailable);
    CHECK_FALSE(released.prepared);
}

namespace {

using pulp::gpu_audio::test_detail::RealtimeHookNode;

// Test-only node: out = gain * in. Deterministic, no GPU — exercises the
// transport's scheduling, latency, and miss handling.
class GainNode : public GpuAudioNode {
  public:
    GainNode(uint32_t channels, uint32_t block, float gain, MissPolicy mp, uint32_t latency = 2,
             bool supports_fallback = true)
        : channels_(channels), block_(block), gain_(gain), mp_(mp), latency_(latency),
          supports_fallback_(supports_fallback) {}

    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "gain";
        d.input_channels = channels_;
        d.output_channels = channels_;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = mp_;
        d.supports_cpu_fallback = supports_fallback_;
        return d;
    }
    bool prepare() override {
        return true;
    }

    bool cpu_fallback_prepared() const noexcept override {
        return supports_fallback_ && mp_ == MissPolicy::CpuFallback;
    }
    void process_block(const BufferView<const float>& in, BufferView<float>& out,
                       uint32_t n) override {
        for (uint32_t c = 0; c < channels_; ++c) {
            const float* s = in.channel_ptr(c);
            float* d = out.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i)
                d[i] = s[i] * gain_;
        }
    }

  private:
    uint32_t channels_, block_;
    float gain_;
    MissPolicy mp_;
    uint32_t latency_;
    bool supports_fallback_;
};

// A staged node that opts into worker pipelining through a link it detaches
// when destroyed, as GpuConvolver does: hosted, it declares a one-block worker
// pipeline, so the transport keeps it opted in until release(). `hosted`
// lives in the node, so a transport reaching a destroyed node writes freed
// memory (ASan reports it).
class PipelineGainNode : public GainNode {
  public:
    PipelineGainNode(uint32_t channels, uint32_t block)
        : GainNode(channels, block, 1.0f, MissPolicy::CpuFallback),
          link_(std::make_shared<detail::WorkerPipelineLink>(this, &PipelineGainNode::set_hosted)) {
    }
    ~PipelineGainNode() override {
        link_->detach();
    }
    GpuAudioNodeDescriptor descriptor() const override {
        auto d = GainNode::descriptor();
        d.worker_pipeline_blocks = hosted ? 1u : 0u;
        return d;
    }
    bool bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept override {
        binding.pipeline = link_;
        return true;
    }
    bool hosted = false;

  private:
    static bool set_hosted(void* self, bool hosted) noexcept {
        static_cast<PipelineGainNode*>(self)->hosted = hosted;
        return true;
    }
    std::shared_ptr<detail::WorkerPipelineLink> link_;
};

// A pipelining node that really emits block N - 1 while hosted, as the staged
// GpuConvolver does: each worker call outputs the previous call's input
// times `gain`, so the transport must prime one block fewer and still deliver
// wet(input from latency_blocks ago).
class DelayedPipelineNode : public GpuAudioNode {
  public:
    DelayedPipelineNode(uint32_t block, uint32_t latency, float gain)
        : block_(block), latency_(latency), gain_(gain), held_(block, 0.0f),
          link_(std::make_shared<detail::WorkerPipelineLink>(this,
                                                             &DelayedPipelineNode::set_hosted)) {}
    ~DelayedPipelineNode() override {
        link_->detach();
    }
    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "delayed-pipeline";
        d.input_channels = d.output_channels = 1;
        d.block_size = block_;
        d.sample_rate = 48000;
        d.latency_blocks = latency_;
        d.miss_policy = MissPolicy::PassthroughDry;
        d.worker_pipeline_blocks = hosted_ ? 1u : 0u;
        return d;
    }
    bool prepare() override {
        return true;
    }
    void process_block(const BufferView<const float>& in, BufferView<float>& out,
                       uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            const float next = in.channel_ptr(0)[i];
            out.channel_ptr(0)[i] = hosted_ ? held_[i] * gain_ : next * gain_;
            held_[i] = next;
        }
    }
    bool bind_realtime_path(detail::RealtimeGpuNodeBinding& binding) noexcept override {
        binding.pipeline = link_;
        return true;
    }

  private:
    static bool set_hosted(void* self, bool hosted) noexcept {
        static_cast<DelayedPipelineNode*>(self)->hosted_ = hosted;
        return true;
    }
    uint32_t block_, latency_;
    float gain_;
    bool hosted_ = false;
    std::vector<float> held_;
    std::shared_ptr<detail::WorkerPipelineLink> link_;
};

// Per-channel storage with stable float / const-float pointer arrays.
struct Block {
    Block(uint32_t ch, uint32_t n)
        : storage(ch, std::vector<float>(n, 0.0f)), ptrs(ch), cptrs(ch), n(n) {
        for (uint32_t c = 0; c < ch; ++c) {
            ptrs[c] = storage[c].data();
            cptrs[c] = storage[c].data();
        }
    }
    void fill(float v) {
        for (auto& ch : storage)
            std::fill(ch.begin(), ch.end(), v);
    }
    BufferView<float> view() {
        return BufferView<float>(ptrs.data(), ptrs.size(), n);
    }
    BufferView<const float> cview() {
        return BufferView<const float>(cptrs.data(), cptrs.size(), n);
    }
    std::vector<std::vector<float>> storage;
    std::vector<float*> ptrs;
    std::vector<const float*> cptrs;
    uint32_t n;
};

struct TrialDeliveryCapture {
    struct Entry {
        std::uint64_t sequence = 0;
        std::uint8_t disposition = 0;
        std::uint64_t callback_start_ns = 0;
        std::uint64_t callback_end_ns = 0;
        std::uint64_t result_visible_ns = 0;
    };
    std::array<Entry, 16> entries{};
    std::uint32_t count = 0;

    static void observe(void* context, std::uint64_t sequence, std::uint8_t disposition,
                        std::uint64_t callback_start_ns, std::uint64_t callback_end_ns,
                        std::uint64_t result_visible_ns) noexcept {
        auto& capture = *static_cast<TrialDeliveryCapture*>(context);
        if (capture.count >= capture.entries.size())
            return;
        capture.entries[capture.count++] =
            Entry{sequence, disposition, callback_start_ns, callback_end_ns, result_visible_ns};
    }
};

} // namespace

TEST_CASE("GpuAudioTransport trial observer records staged callback delivery",
          "[gpu_audio][transport][trace]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    TrialDeliveryCapture capture;
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {.ring_blocks = RING}));
    REQUIRE(
        configure_gpu_audio_transport_trial_observer(t, &capture, &TrialDeliveryCapture::observe));

    Block in(CH, BS), out(CH, BS);
    for (int k = 0; k < 4; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto output = out.view();
        t.process(in.cview(), output, BS);
        t.pump();
    }

    REQUIRE(capture.count == 4);
    for (std::uint32_t i = 0; i < capture.count; ++i) {
        CHECK(capture.entries[i].sequence == i);
        const auto expected = i < L ? detail::SharedIoDeliveryDisposition::Priming
                                    : detail::SharedIoDeliveryDisposition::GpuDelivered;
        CHECK(capture.entries[i].disposition == static_cast<std::uint8_t>(expected));
        CHECK(capture.entries[i].callback_start_ns != 0);
        CHECK(capture.entries[i].callback_end_ns >= capture.entries[i].callback_start_ns);
        CHECK(capture.entries[i].callback_end_ns != 0);
        CHECK(capture.entries[i].result_visible_ns >= capture.entries[i].callback_end_ns);
    }
}

TEST_CASE("GpuAudioTransport trial observer records fallback dispositions",
          "[gpu_audio][transport][trace]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    for (const auto policy : {MissPolicy::CpuFallback, MissPolicy::Silence}) {
        GainNode node(CH, BS, 2.0f, policy, L);
        REQUIRE(node.prepare());
        TrialDeliveryCapture capture;
        GpuAudioTransport t;
        REQUIRE(t.prepare(&node, {.ring_blocks = RING}));
        REQUIRE(configure_gpu_audio_transport_trial_observer(t, &capture,
                                                             &TrialDeliveryCapture::observe));

        Block in(CH, BS), out(CH, BS);
        for (int k = 0; k < 3; ++k) {
            in.fill(static_cast<float>(k + 1));
            auto output = out.view();
            t.process(in.cview(), output, BS);
        }

        REQUIRE(capture.count == 3);
        const auto fallback = policy == MissPolicy::CpuFallback
                                  ? detail::SharedIoDeliveryDisposition::CpuFallbackDelivered
                                  : detail::SharedIoDeliveryDisposition::SilenceDelivered;
        for (std::uint32_t i = 0; i < capture.count; ++i) {
            const auto expected = i < L ? detail::SharedIoDeliveryDisposition::Priming : fallback;
            CHECK(capture.entries[i].disposition == static_cast<std::uint8_t>(expected));
        }
    }
}

TEST_CASE("GpuAudioTransport applies fixed latency + node processing", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));
    REQUIRE(t.latency_samples() == L * BS);

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    constexpr int NBLK = 10;
    for (int k = 0; k < NBLK; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS); // RT: write input, read delayed output
        t.pump();              // worker keeps pace (one block per call)
        first_sample.push_back(out.storage[0][0]);
    }

    for (int k = 0; k < NBLK; ++k) {
        const float expected =
            (k >= static_cast<int>(L)) ? 2.0f * static_cast<float>(k - L + 1) : 0.0f;
        REQUIRE(first_sample[k] == expected);
    }
    REQUIRE(t.stats().miss_blocks == 0);
    REQUIRE(t.stats().produced_blocks == NBLK);
}

TEST_CASE("GpuAudioTransport miss policy fills dry on starvation", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    for (int k = 0; k < 4; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS); // NEVER pump → worker starved
        first_sample.push_back(out.storage[0][0]);
    }

    REQUIRE(first_sample[0] == 0.0f);
    REQUIRE(first_sample[1] == 0.0f);
    REQUIRE(first_sample[2] == 3.0f); // dry input value k+1 = 3
    REQUIRE(first_sample[3] == 4.0f);
    REQUIRE(t.stats().miss_blocks == 2);
}

TEST_CASE("GpuAudioTransport process_offline drives the node synchronously",
          "[gpu_audio][transport]") {
    // Offline render (e.g. a faster-than-real-time bounce): NO worker thread and
    // we NEVER call pump() manually — process_offline() must pump the node inline
    // so every block is captured. This is exactly the case where the realtime
    // process() path starves and misses (see the miss-policy test above); here the
    // miss policy is Silence so any drop would show up as a zero, yet none occur.
    // Latency stays identical to process() (the primed output ring).
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::Silence, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING})); // run_worker_thread defaults to false
    REQUIRE(t.latency_samples() == L * BS);

    Block in(CH, BS), out(CH, BS);
    std::vector<float> first_sample;
    constexpr int NBLK = 10;
    for (int k = 0; k < NBLK; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process_offline(iv, ov, BS); // synchronous: no worker, no manual pump
        first_sample.push_back(out.storage[0][0]);
    }

    // Same latency-delayed gain output as the worker-paced realtime case, but with
    // ZERO misses — proving the node ran for every block under offline drive.
    for (int k = 0; k < NBLK; ++k) {
        const float expected =
            (k >= static_cast<int>(L)) ? 2.0f * static_cast<float>(k - L + 1) : 0.0f;
        REQUIRE(first_sample[k] == expected);
    }
    REQUIRE(t.stats().miss_blocks == 0);
    REQUIRE(t.stats().produced_blocks == NBLK);
}

TEST_CASE("GpuAudioTransport drops input on a full ring (no block)", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, RING = 4;
    GainNode node(CH, BS, 1.0f, MissPolicy::Silence, 2);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(1.0f);
    for (int k = 0; k < 20; ++k) {
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
    }
    REQUIRE(t.stats().input_dropped_frames > 0);
}

TEST_CASE("GpuAudioTransport rejects invalid descriptor / config", "[gpu_audio][transport]") {
    // Zero channels.
    {
        GainNode bad(0, 64, 1.0f, MissPolicy::Silence, 2);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&bad, {8}));
        REQUIRE_FALSE(t.is_prepared());
    }
    // ring_blocks too small for latency (needs latency + 2).
    {
        GainNode node(1, 64, 1.0f, MissPolicy::Silence, 4);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {5})); // 5 < 4 + 2
        REQUIRE(t.prepare(&node, {6}));       // ok
    }
    // CpuFallback miss policy without a real fallback.
    {
        GainNode node(1, 64, 1.0f, MissPolicy::CpuFallback, 2, /*supports_fallback=*/false);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {8}));
    }
    // A declaration without a prepared witness also fails closed.
    {
        struct UnpreparedFallbackNode final : GainNode {
            using GainNode::GainNode;
            bool cpu_fallback_prepared() const noexcept override {
                return false;
            }
        } node(1, 64, 1.0f, MissPolicy::CpuFallback, 2);
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(&node, {8}));
    }
    // null node.
    {
        GpuAudioTransport t;
        REQUIRE_FALSE(t.prepare(nullptr, {8}));
    }
}

TEST_CASE("GpuAudioTransport background worker drains the pipeline", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 32;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING, /*run_worker_thread=*/true}));

    Block in(CH, BS), out(CH, BS);
    constexpr int N = 200;
    for (int k = 0; k < N; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        std::this_thread::sleep_for(std::chrono::microseconds(300)); // ~ worker poll pace
    }

    // Liveness: give the worker a bounded grace period to drain the backlog.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = t.stats();
        if (s.produced_blocks + s.input_dropped_frames / BS >= static_cast<std::uint64_t>(N))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = t.stats();
    REQUIRE(s.produced_blocks > 0); // the worker actually ran
    // Exact, timing-independent invariant: every fed block was produced or
    // dropped whole (no partial/misaligned blocks).
    REQUIRE(s.produced_blocks + s.input_dropped_frames / BS == static_cast<std::uint64_t>(N));
    // A resync can only ever cancel a prior miss's late wet block, so the count
    // of resynced blocks can never exceed the misses. A depth-based resync would
    // violate this under worker timing (it drains a block the worker raced ahead
    // to produce even with zero misses); the miss-counting resync cannot.
    REQUIRE(s.resynced_blocks <= s.miss_blocks);

    // release() must cleanly stop + join the worker (no hang / no crash).
    t.release();
    REQUIRE_FALSE(t.is_prepared());
}

#if defined(__APPLE__)
TEST_CASE("GpuAudioTransport can opt its worker into an Audio Workgroup",
          "[gpu_audio][transport][workgroup][rt-safety]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 16;
    auto* workgroup = os_workgroup_parallel_create("pulp-gpu-audio-worker", nullptr);
    REQUIRE(workgroup != nullptr);

    GainNode node(CH, BS, 2.0f, MissPolicy::CpuFallback, L);
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {.ring_blocks = RING,
                                      .run_worker_thread = true,
                                      .wake_on_write = true,
                                      .audio_workgroup = reinterpret_cast<void*>(workgroup),
                                      .join_audio_workgroup = true}));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    GpuAudioTransport::Stats observed;
    do {
        observed = transport.stats();
        if (observed.worker_workgroup_joined || observed.worker_workgroup_join_failures > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    REQUIRE(observed.worker_workgroup_joined);
    CHECK(observed.worker_workgroup_join_failures == 0);
    transport.release();
    os_release(workgroup);
}
#endif

TEST_CASE("GpuAudioTransport resyncs the wet timeline after a miss", "[gpu_audio][transport]") {
    // A miss emits a substitute (dry) block for its timeline slot; when the worker
    // later catches up it back-fills that slot's wet block, pushing the output ring
    // above the primed steady-state depth. Without a resync the RT read would pick
    // up that stale wet block, permanently delaying the wet stream one block per
    // miss (a comb filter of dry against a one-block-late wet). This drives a
    // deterministic miss-then-catch-up sequence with manual pump control and proves
    // the resync drops the redundant block so effective latency stays pinned.
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING})); // no worker thread — we drive pump()

    Block in(CH, BS), out(CH, BS);
    std::vector<float> got;
    constexpr int NBLK = 8;
    for (int k = 0; k < NBLK; ++k) {
        // Worker catches up BEFORE the callback from k>=6 on: drain the backlog
        // (including the late wet block for the missed slot) so the RT read sees
        // the over-filled ring the resync must correct.
        if (k >= 6)
            t.pump();
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        // Keep pace through call 2; starve calls 3-5 (drains the 2-block cushion,
        // forcing exactly one miss at call 5); catch-up handled above from k>=6.
        if (k <= 2)
            t.pump();
        got.push_back(out.storage[0][0]);
    }

    // out[5] is the dry substitute (input 6) — the one block the miss cost us.
    // out[6] is realigned to the no-miss value (10), NOT the stale late wet (8).
    const std::vector<float> expected = {0, 0, 2, 4, 6, /*miss→dry*/ 6, /*resynced*/ 10, 12};
    REQUIRE(got == expected);
    REQUIRE(t.stats().miss_blocks == 1);
    REQUIRE(t.stats().resynced_blocks == 1); // exactly the one late wet block dropped
}

TEST_CASE("GpuAudioTransport rejects the sole stale result after deadline misses",
          "[gpu_audio][transport]") {
    for (const uint32_t misses : {1u, 3u}) {
        INFO("prior misses=" << misses);
        constexpr uint32_t BS = 32;
        GainNode node(1, BS, 2.0f, MissPolicy::PassthroughDry, 1);
        REQUIRE(node.prepare());
        GpuAudioTransport transport;
        REQUIRE(transport.prepare(&node, {16}));
        Block in(1, BS), out(1, BS);
        auto callback = [&](float value) {
            in.fill(value);
            auto iv = in.cview();
            auto ov = out.view();
            transport.process(iv, ov, BS);
            return out.storage[0][0];
        };
        REQUIRE(callback(1.0f) == 0.0f);
        for (uint32_t i = 0; i < misses; ++i)
            REQUIRE(callback(static_cast<float>(i + 2)) == static_cast<float>(i + 2));

        // Only input 1 completes. Its deadline already passed, so the callback
        // must substitute again, even though discarding it empties the ring.
        transport.pump(1);
        const float current = static_cast<float>(misses + 2);
        CHECK(callback(current) == current);
        CHECK(transport.stats().resynced_blocks == 1);
        CHECK(transport.stats().miss_blocks == misses + 1);

        // Complete the backlog; precisely the previous callback's input is due.
        transport.pump();
        CHECK(callback(current + 1.0f) == 2.0f * current);
        CHECK(transport.stats().resynced_blocks == misses + 1);
    }
}

TEST_CASE("GpuAudioTransport keeps latency alignment after dropped input blocks",
          "[gpu_audio][transport]") {
    // The worker stalls after callback 9. Callbacks 12-15 miss, and inputs 14
    // and 15 find the input ring full, so their wet blocks never exist. Once
    // the worker catches up, slots 14 and 15 must still be substituted and
    // every later callback must return the input from exactly L blocks ago.
    constexpr uint32_t BS = 32, L = 2, RING = 4;
    GainNode node(1, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(1, BS), out(1, BS);
    for (int k = 0; k < 24; ++k) {
        INFO("callback " << k);
        if (k == 16)
            t.pump();
        const float value = static_cast<float>(k + 1);
        in.fill(value);
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        if (k <= 9 || k >= 16)
            t.pump();
        float expected = 2.0f * static_cast<float>(k - 1); // wet input k - L
        if (k < static_cast<int>(L))
            expected = 0.0f;
        else if (k >= 12 && k <= 17)
            expected = value; // missed, or its input was dropped: dry substitute
        CHECK(out.storage[0][0] == expected);
    }
    CHECK(t.stats().input_dropped_frames == 2 * BS);
    CHECK(t.stats().miss_blocks == 6);
    CHECK(t.stats().resynced_blocks == 4);
}

TEST_CASE("GpuAudioTransport keeps a pipelining node aligned after dropped input blocks",
          "[gpu_audio][transport][pipeline]") {
    // As the drop test above, on a node that emits block N - 1 while hosted.
    // Every output is either the dry substitute for its own callback or the
    // wet block from exactly L callbacks ago; once the backlog drains, every
    // block is wet and aligned.
    constexpr uint32_t BS = 32, L = 3, RING = 5;
    DelayedPipelineNode node(BS, L, 2.0f);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));
    REQUIRE(node.descriptor().worker_pipeline_blocks == 1);

    Block in(1, BS), out(1, BS);
    int misaligned = 0, wet_tail = 0;
    constexpr int kCallbacks = 40, kStallFrom = 10, kResumeAt = 18;
    for (int k = 0; k < kCallbacks; ++k) {
        INFO("callback " << k);
        if (k == kResumeAt)
            t.pump();
        const float value = static_cast<float>(k + 1);
        in.fill(value);
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        if (k < kStallFrom || k >= kResumeAt)
            t.pump();
        const float got = out.storage[0][0];
        const float wet = 2.0f * static_cast<float>(k + 1 - static_cast<int>(L));
        const bool aligned = k < static_cast<int>(L) ? got == 0.0f : got == wet;
        if (!aligned && got != value)
            ++misaligned;
        if (k >= kCallbacks - 10 && aligned)
            ++wet_tail;
    }
    CHECK(misaligned == 0);
    CHECK(wet_tail == 10);
    CHECK(t.stats().input_dropped_frames > 0);
}

TEST_CASE("GpuAudioTransport wake-on-write worker drains the pipeline", "[gpu_audio][transport]") {
    // Same whole-block conservation invariant as the polling worker, but with the
    // opt-in wake-on-write path: process() posts a semaphore the worker waits on.
    // Proves the semaphore-driven worker still drains every fed block and that
    // release() cleanly joins it.
    constexpr uint32_t CH = 2, BS = 64, L = 2, RING = 32;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, L);
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING, /*run_worker_thread=*/true, /*wake_on_write=*/true}));

    Block in(CH, BS), out(CH, BS);
    constexpr int N = 200;
    for (int k = 0; k < N; ++k) {
        in.fill(static_cast<float>(k + 1));
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = t.stats();
        if (s.produced_blocks + s.input_dropped_frames / BS >= static_cast<std::uint64_t>(N))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto s = t.stats();
    REQUIRE(s.produced_blocks > 0);
    REQUIRE(s.produced_blocks + s.input_dropped_frames / BS == static_cast<std::uint64_t>(N));

    t.release();
    REQUIRE_FALSE(t.is_prepared());
}

TEST_CASE("GpuAudioTransport silences mismatched / too-small views", "[gpu_audio][transport]") {
    constexpr uint32_t CH = 2, BS = 64, RING = 8;
    GainNode node(CH, BS, 2.0f, MissPolicy::PassthroughDry, 2);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(1.0f);
    out.fill(7.0f); // sentinel — must be overwritten with silence

    // Wrong block size.
    auto iv = in.cview();
    auto ov = out.view();
    t.process(iv, ov, BS / 2);
    REQUIRE(out.storage[0][0] == 0.0f);

    // Too few output channels.
    Block mono_out(1, BS);
    mono_out.fill(7.0f);
    auto mov = mono_out.view();
    t.process(iv, mov, BS);
    REQUIRE(mono_out.storage[0][0] == 0.0f);
}

// A node that declares nothing about its miss behavior must not pass audio
// through on a miss. Passing through is the WORST default here: the transport's
// output is delayed by latency_blocks, so a dry sample for the CURRENT time jumps
// the stream forward by the whole latency for one block and back again -- a
// timeline break rather than an honest dropout. And a node that never thought
// about misses is exactly the node whose CPU fallback is not correct.
TEST_CASE("GpuAudioNodeDescriptor defaults fail closed", "[gpu_audio][transport]") {
    GpuAudioNodeDescriptor d;
    REQUIRE(d.miss_policy == MissPolicy::Silence);
    REQUIRE_FALSE(d.supports_cpu_fallback);
}

TEST_CASE("GpuAudioTransport starves to silence when the node declares no policy",
          "[gpu_audio][transport]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;

    // A node whose descriptor leaves miss_policy and supports_cpu_fallback at
    // their defaults -- the shape a first-time GPU node author writes.
    struct DefaultPolicyNode : GpuAudioNode {
        GpuAudioNodeDescriptor descriptor() const override {
            GpuAudioNodeDescriptor d; // defaults, deliberately untouched
            d.name = "defaults";
            d.input_channels = CH;
            d.output_channels = CH;
            d.block_size = BS;
            d.sample_rate = 48000;
            d.latency_blocks = L;
            return d;
        }
        bool prepare() override {
            return true;
        }
        void process_block(const BufferView<const float>&, BufferView<float>& out,
                           uint32_t n) override {
            for (uint32_t i = 0; i < n; ++i)
                out.channel_ptr(0)[i] = 1.0f;
        }
    };

    DefaultPolicyNode node;
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    // Never pump the worker, so every read is a miss. The dry input is 0.5 --
    // any nonzero output here is the dry signal leaking through on a miss.
    Block in(CH, BS), out(CH, BS);
    for (int k = 0; k < 4; ++k) {
        in.fill(0.5f);
        auto iv = in.cview();
        auto ov = out.view();
        t.process(iv, ov, BS);
        for (uint32_t i = 0; i < BS; ++i) {
            INFO("block " << k << " sample " << i);
            REQUIRE(out.storage[0][i] == 0.0f);
        }
    }
    // Misses were recorded (the exact count depends on the transport's priming
    // and resync bookkeeping; what this test pins is that they came out SILENT).
    REQUIRE(t.stats().miss_blocks >= 1);
}

TEST_CASE("GpuAudioTransport rejected callbacks preserve the fallback impulse position",
          "[gpu_audio][transport][realtime-path][callback-timeline]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.stateful_fallback = true;
    node.enforce_sequence = true;
    node.realtime_status = detail::kRealtimeGpuMissed;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    auto in = input.cview();
    auto out = output.view();

    bool invalid = false;
    bool offline = false;
    SECTION("valid silent callback") {}
    SECTION("invalid realtime callback") {
        invalid = true;
    }
    SECTION("invalid offline callback") {
        invalid = true;
        offline = true;
    }

    input.storage[0][0] = 1.0f;
    transport.process(in, out, BS);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    input.fill(invalid ? 9.0f : 0.0f);
    output.fill(7.0f);
    node.external_output_sample = output.storage[0].data();
    if (offline)
        transport.process_offline(in, out, invalid ? BS / 2 : BS);
    else
        transport.process(in, out, invalid ? BS / 2 : BS);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    CHECK(node.prime_fallback_calls == 2);
    CHECK(node.next_sequence == 2);
    CHECK(node.invalid_callback_calls == (invalid ? 1u : 0u));
    CHECK(node.invalid_input_zero_filled);
    CHECK(node.callback_shape_valid);
    CHECK(node.external_output_silent_on_rejection);
    if (invalid)
        CHECK(node.delivery == detail::SharedIoDeliveryDisposition::InvalidRejected);

    input.fill(0.0f);
    transport.process(in, out, BS);
    for (uint32_t sample = 0; sample < BS; ++sample)
        CHECK(output.storage[0][sample] == (sample == 0 ? 1.0f : 0.0f));
    transport.process(in, out, BS);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    CHECK(node.prime_fallback_calls == 4);
    CHECK(node.next_sequence == 4);
    CHECK(node.stale_callback_calls == 0);
    CHECK(node.sequence_gap_calls == 0);
}

TEST_CASE("GpuAudioTransport rejected realtime views use prepared allocation-free storage",
          "[gpu_audio][transport][realtime-path][rt-safety]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.stateful_fallback = true;
    node.enforce_sequence = true;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS), short_input(1, BS / 2), short_output(1, BS / 2);
    input.fill(9.0f);
    short_input.fill(9.0f);
    output.fill(9.0f);
    short_output.fill(9.0f);
    auto in = input.cview();
    auto out = output.view();
    auto short_in = short_input.cview();
    auto short_out = short_output.view();
    BufferView<const float> no_input_channels(input.cptrs.data(), 0, BS);
    BufferView<float> no_output_channels(output.ptrs.data(), 0, BS);
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        transport.process(in, out, BS / 2);
        transport.process(short_in, out, BS);
        transport.process(in, short_out, BS);
        transport.process(no_input_channels, out, BS);
        transport.process(in, no_output_channels, BS);
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
    CHECK(node.invalid_callback_calls == 5);
    CHECK(node.prime_fallback_calls == 5);
    CHECK(node.next_sequence == 5);
    CHECK(node.invalid_input_zero_filled);
    CHECK(node.callback_shape_valid);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::InvalidRejected);
    CHECK(node.stale_callback_calls == 0);
    CHECK(node.sequence_gap_calls == 0);
    for (float sample : output.storage[0])
        CHECK(sample == 0.0f);
    for (float sample : short_output.storage[0])
        CHECK(sample == 0.0f);
}

TEST_CASE("GpuAudioTransport retains the prepared node timeline across transport lifetimes",
          "[gpu_audio][transport][realtime-path][callback-timeline]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.enforce_sequence = true;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    auto in = input.cview();
    auto out = output.view();
    transport.process(in, out, BS);
    transport.process(in, out, BS);
    REQUIRE(node.next_sequence == 2);

    SECTION("reprepare the same transport") {
        REQUIRE(transport.prepare(&node, {8}));
        transport.process(in, out, BS);
    }
    SECTION("attach a replacement transport after release") {
        transport.release();
        GpuAudioTransport replacement;
        REQUIRE(replacement.prepare(&node, {8}));
        replacement.process(in, out, BS);
    }

    CHECK(node.last_sequence == 2);
    CHECK(node.next_sequence == 3);
    CHECK(node.next_sequence_queries == 2);
    CHECK(node.delivered_sequence == 2);
    CHECK(node.stale_callback_calls == 0);
    CHECK(node.sequence_gap_calls == 0);
    CHECK_FALSE(node.sequence_fenced);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::GpuDelivered);
    CHECK(output.storage[0][0] == 1000.0f);
}

TEST_CASE("GpuAudioTransport offline fence retains fallback and the monotonic timeline",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t CH = 1, BS = 32;
    RealtimeHookNode node(CH, BS, MissPolicy::CpuFallback);
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(CH, BS), output(CH, BS);
    input.fill(5.0f);
    auto in = input.cview();
    auto out = output.view();
    transport.process(in, out, BS);
    CHECK(node.last_sequence == 0);
    CHECK(output.storage[0][0] == 1005.0f);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::GpuDelivered);
    transport.process(in, out, BS / 2); // invalid calls still consume absolute sequence
    CHECK(node.realtime_calls == 2);
    CHECK(output.storage[0][0] == 0.0f);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::InvalidRejected);
    transport.process_offline(in, out, BS);
    CHECK(node.last_sequence == 2);
    CHECK(node.delivered_sequence == 2);
    CHECK(node.fence_calls == 1);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::CpuFallbackDelivered);
    CHECK(output.storage[0][0] == -5.0f);
    transport.process(in, out, BS);
    CHECK(node.last_sequence == 3);
    CHECK(output.storage[0][0] == -5.0f);
    transport.process_offline(in, out, BS);
    CHECK(node.last_sequence == 4);
    CHECK(node.fence_calls == 1);
    CHECK(node.prime_fallback_calls == 5);
    CHECK(node.delivery_calls == 5);
    CHECK(node.process_block_calls == 0);
}

TEST_CASE("GpuAudioTransport failed offline drain retains hooks and never starts staged GPU work",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.fence_result = false;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    input.fill(7.0f);
    auto in = input.cview();
    auto out = output.view();
    transport.process_offline(in, out, BS);
    CHECK(node.fence_calls == 1);
    CHECK(output.storage[0][0] == -7.0f);
    transport.process(in, out, BS);
    transport.pump();
    CHECK(node.service_calls == 1);
    CHECK(node.process_block_calls == 0);
    CHECK(output.storage[0][0] == -7.0f);
    node.fence_result = true;
    transport.process_offline(in, out, BS);
    CHECK(node.fence_calls == 2);
    CHECK(node.last_sequence == 2);
    CHECK(node.prime_fallback_calls == 3);
    CHECK(node.cpu_fallback_calls == 3);
    CHECK(node.delivery_calls == 3);
    CHECK(node.process_block_calls == 0);
}

TEST_CASE("GpuAudioTransport inactive private provider retains CPU-only fallback",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t BS = 32;
    RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
    node.realtime_status = detail::kRealtimeGpuInactive;
    node.service_return_blocks = detail::kRealtimeGpuServiceInactive;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    Block input(1, BS), output(1, BS);
    input.fill(2.0f);
    auto in = input.cview();
    auto out = output.view();
    transport.process(in, out, BS);
    CHECK(output.storage[0][0] == -2.0f);
    transport.pump();
    CHECK(node.service_calls == 1);
    CHECK(node.process_block_calls == 0);
    CHECK(node.prime_fallback_calls == 1);
    CHECK(node.cpu_fallback_calls == 1);
    CHECK(node.delivery == detail::SharedIoDeliveryDisposition::CpuFallbackDelivered);
}

TEST_CASE("GpuAudioTransport private realtime miss uses the declared miss policy",
          "[gpu_audio][transport][realtime-path]") {
    constexpr uint32_t CH = 1, BS = 32, L = 2, RING = 8;
    RealtimeHookNode node(CH, BS, MissPolicy::PassthroughDry, L);
    node.realtime_status = detail::kRealtimeGpuMissed;
    REQUIRE(node.prepare());

    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {RING}));

    Block in(CH, BS), out(CH, BS);
    in.fill(42.0f);
    auto iv = in.cview();
    auto ov = out.view();
    t.process(iv, ov, BS);

    REQUIRE(out.storage[0][0] == 42.0f);
    REQUIRE(node.realtime_calls == 1);
    REQUIRE(node.process_block_calls == 0);
    REQUIRE(t.stats().miss_blocks == 1);
}

namespace {
std::uint64_t delivery_total(const GpuAudioTransport::DeliverySnapshot& s) {
    return s.gpu_blocks + s.worker_output_blocks + s.cpu_fallback_blocks + s.silence_blocks +
           s.passthrough_blocks + s.priming_blocks + s.invalid_blocks;
}
} // namespace

TEST_CASE("GpuAudioTransport public delivery snapshot counts selected callback output",
          "[gpu_audio][transport][delivery-snapshot]") {
    for (auto policy : {MissPolicy::CpuFallback, MissPolicy::Silence, MissPolicy::PassthroughDry}) {
        RealtimeHookNode node(1, 32, policy);
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        REQUIRE(t.prepare(&node, {8}));
        Block input(1, 32), output(1, 32);
        input.fill(3.0f);
        auto in = input.cview();
        auto out = output.view();
        std::size_t allocations;
        {
            pulp::test::RtAllocationProbe probe;
            t.process(in, out, 32);
            allocations = probe.allocation_count();
        }
        CHECK(allocations == 0);
        CHECK(output.storage[0][0] == 1003.0f);
        CHECK(t.delivery_snapshot().gpu_blocks == 1);
        node.realtime_status = detail::kRealtimeGpuPriming;
        t.process(in, out, 32);
        CHECK(output.storage[0][0] == 0.0f);
        CHECK(t.delivery_snapshot().priming_blocks == 1);
        node.realtime_status = detail::kRealtimeGpuMissed;
        t.process(in, out, 32);
        const auto selected = t.delivery_snapshot();
        CHECK(selected.cpu_fallback_blocks == (policy == MissPolicy::CpuFallback ? 1u : 0u));
        CHECK(selected.silence_blocks == (policy == MissPolicy::Silence ? 1u : 0u));
        CHECK(selected.passthrough_blocks == (policy == MissPolicy::PassthroughDry ? 1u : 0u));
        CHECK(output.storage[0][0] == (policy == MissPolicy::CpuFallback ? -3.0f
                                       : policy == MissPolicy::Silence   ? 0.0f
                                                                         : 3.0f));
        node.realtime_status = detail::kRealtimeGpuReady;
        t.process(in, out, 16);
        CHECK(output.storage[0][0] == 0.0f);
        const auto invalid = t.delivery_snapshot();
        CHECK(invalid.invalid_blocks == 1);
        CHECK(invalid.gpu_blocks == 1);
        CHECK(invalid.worker_output_blocks == 0);
        CHECK(delivery_total(invalid) == 4);
        t.process_offline(in, out, 32);
        CHECK(delivery_total(t.delivery_snapshot()) == 4);
        t.release();
        t.process(in, out, 32);
        CHECK(delivery_total(t.delivery_snapshot()) == 4);
        CHECK_FALSE(t.prepare(nullptr, {8}));
        CHECK(delivery_total(t.delivery_snapshot()) == 4);
        REQUIRE(t.prepare(&node, {8}));
        CHECK(delivery_total(t.delivery_snapshot()) == 0);
    }
}

TEST_CASE("GpuAudioTransport ring output never claims GPU execution or stale delivery",
          "[gpu_audio][transport][delivery-snapshot]") {
    GainNode node(1, 32, 2.0f, MissPolicy::PassthroughDry, 1);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {8}));
    Block input(1, 32), output(1, 32);
    input.fill(3.0f);
    auto in = input.cview();
    auto out = output.view();
    t.process(in, out, 32);
    CHECK(t.delivery_snapshot().priming_blocks == 1);
    t.process(in, out, 32);
    t.pump(1);
    t.process(in, out, 32);
    CHECK(output.storage[0][0] == 3.0f);
    CHECK(t.delivery_snapshot().worker_output_blocks == 0);
    CHECK(t.delivery_snapshot().passthrough_blocks == 2);
    t.pump();
    t.process(in, out, 32);
    CHECK(output.storage[0][0] == 6.0f);
    CHECK(t.delivery_snapshot().worker_output_blocks == 1);
    CHECK(t.delivery_snapshot().gpu_blocks == 0);
    t.process(in, out, 16);
    CHECK(t.delivery_snapshot().invalid_blocks == 1);
    CHECK(delivery_total(t.delivery_snapshot()) == 5);
    t.process_offline(in, out, 32);
    CHECK(delivery_total(t.delivery_snapshot()) == 5);
}

TEST_CASE("GpuAudioTransport status snapshot is stable before and after prepare",
          "[gpu_audio][transport][status]") {
    GainNode node(1, 32, 1.0f, MissPolicy::CpuFallback, 2);
    GpuAudioTransport t;

    const auto before = t.status_snapshot();
    CHECK(validate_gpu_audio_status(before).accepted());
    CHECK(before.schema_version == GpuAudioStatus::kSchemaVersion);
    CHECK(before.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(before.selected_engine == GpuAudioEngine::Unknown);

    REQUIRE(node.prepare());
    REQUIRE(t.prepare(&node, {8}));
    const auto after = t.status_snapshot();
    CHECK(validate_gpu_audio_status(after).accepted());
    // A generic staged node does not prove a GPU provider. Its worker may be
    // CPU-backed, so status must remain honest instead of claiming GPU/Ready.
    CHECK(after.provider_state == GpuAudioProviderState::Degraded);
    CHECK(after.selected_engine == GpuAudioEngine::Unknown);
    CHECK(after.provider == GpuAudioProvider::Unknown);
    CHECK(after.sample_rate == 48000);
    CHECK(after.block_size == 32);
    CHECK(after.latency_samples == 64);
    CHECK(after.fallback_available);
    CHECK(after.fallback_policy == MissPolicy::CpuFallback);
}

TEST_CASE("GpuAudioTransport delivery snapshot supports concurrent diagnostic reads",
          "[gpu_audio][transport][delivery-snapshot]") {
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {8}));
    Block input(1, 32), output(1, 32);
    auto in = input.cview();
    auto out = output.view();
    std::atomic<bool> done{false};
    std::atomic<bool> monotonic{true};
    std::atomic<bool> timed_out{false};
    std::thread reader([&] {
        std::uint64_t previous = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            const auto current = t.delivery_snapshot().gpu_blocks;
            if (current < previous)
                monotonic.store(false, std::memory_order_relaxed);
            previous = current;
        }
        timed_out.store(!done.load(std::memory_order_acquire));
    });
    for (int i = 0; i < 10000; ++i)
        t.process(in, out, 32);
    done.store(true, std::memory_order_release);
    reader.join();
    CHECK_FALSE(timed_out.load());
    CHECK(monotonic.load());
    CHECK(t.delivery_snapshot().gpu_blocks == 10000);
    CHECK(delivery_total(t.delivery_snapshot()) == 10000);
}

namespace {

// Every status a health test observes must satisfy the v3 invariants.
GpuAudioStatus validated(const GpuAudioStatus& status) {
    const auto validation = validate_gpu_audio_status(status);
    INFO("status error " << static_cast<int>(validation.error));
    CHECK(validation.accepted());
    return status;
}

constexpr std::size_t slot(GpuAudioTerminalDisposition disposition) noexcept {
    return static_cast<std::size_t>(disposition);
}

struct HealthHarness {
    explicit HealthHarness(bool claims_provider = true, bool expose_health = true)
        : node(1, 32, MissPolicy::CpuFallback), input(1, 32), output(1, 32) {
        node.claims_provider = claims_provider;
        node.expose_health = expose_health;
        // A hook standing in for an authenticated provider also stands in
        // for its prepared kernel, as every shipped GPU path reports one.
        node.health.gpu_kernel_available = claims_provider && expose_health;
        REQUIRE(node.prepare());
        REQUIRE(transport.prepare(&node, {8}));
        input.fill(0.25f);
    }
    void callback() {
        auto in = input.cview();
        auto out = output.view();
        transport.process(in, out, 32);
    }
    RealtimeHookNode node;
    GpuAudioTransport transport;
    Block input;
    Block output;
};

} // namespace

TEST_CASE("GpuAudioTransport status reads a GPU the fallback carries as CPU-delivering",
          "[gpu_audio][transport][status][health]") {
    HealthHarness h;
    const auto run = [&h](int blocks) {
        for (int i = 0; i < blocks; ++i)
            h.callback();
    };
    // Positive control: the GPU delivers every block.
    run(40);
    auto status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.selected_engine == GpuAudioEngine::Gpu);

    // Stuck late: every block misses and the CPU fallback carries it, with no
    // recovery cause latched. Half the window still on the GPU is Ready.
    h.node.realtime_status = detail::kRealtimeGpuMissed;
    run(16);
    CHECK(validated(h.transport.status_snapshot()).selected_engine == GpuAudioEngine::Gpu);
    run(1);
    status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    run(40);
    CHECK(validated(h.transport.status_snapshot()).selected_engine == GpuAudioEngine::Cpu);
    // Priming blocks are no evidence either way.
    h.node.realtime_status = detail::kRealtimeGpuPriming;
    run(40);
    CHECK(validated(h.transport.status_snapshot()).selected_engine == GpuAudioEngine::Cpu);

    // Recovery: the GPU delivers again and reads Ready once it carries half
    // the window.
    h.node.realtime_status = detail::kRealtimeGpuReady;
    run(15);
    CHECK(validated(h.transport.status_snapshot()).selected_engine == GpuAudioEngine::Cpu);
    run(1);
    status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.selected_engine == GpuAudioEngine::Gpu);
}

TEST_CASE("GpuAudioTransport re-acquire defaults and backoff growth",
          "[gpu_audio][transport][reacquire]") {
    const GpuAudioTransport::Config defaults;
    CHECK(defaults.ring_blocks == 4u);
    CHECK_FALSE(defaults.run_worker_thread);
    CHECK_FALSE(defaults.wake_on_write);
    CHECK(defaults.recovery_policy == GpuAudioRecoveryPolicy::Manual);
    CHECK(defaults.reacquire_release_budget_ns == 250'000'000ull);
    CHECK(defaults.reacquire_max_attempts == 3u);
    CHECK(defaults.reacquire_backoff_ns == 100'000'000ull);

    // Each wait is five times the one before: with a 20 ms first wait, the
    // third attempt may not start until 100 ms after the second. Only that
    // lower bound is checked; an attempt is gated on the monotonic clock, so
    // a loaded host can delay it but never bring it forward.
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
    node.claims_provider = true;
    node.expose_health = true;
    node.expose_reprepare = true;
    node.health.gpu_kernel_available = true;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    constexpr auto kFirstWait = std::chrono::milliseconds(20);
    REQUIRE(transport.prepare(
        &node, {.ring_blocks = 8,
                .recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire,
                .reacquire_max_attempts = 3,
                .reacquire_backoff_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(kFirstWait).count())}));
    // The replacement is lost before it delivers, so every attempt finds an
    // outage and the count never resets.
    const auto lose = [&] {
        node.health.fenced = true;
        node.health.delivery_epoch_active = false;
        node.health.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
        node.health.recovery_sequence = node.next_sequence;
    };
    // Pumps until attempt `n` starts; returns when the pump that ran it began.
    const auto attempt = [&](std::uint32_t n) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto before = std::chrono::steady_clock::now();
            transport.pump();
            if (node.reprepare_calls == n) {
                lose();
                return before;
            }
            std::this_thread::yield();
        }
        FAIL("attempt " << n << " never started");
        return std::chrono::steady_clock::time_point{};
    };
    lose();
    (void)attempt(1);
    const auto second = attempt(2);
    std::uint32_t early_attempts = 0;
    while (std::chrono::steady_clock::now() <
           second + 5 * kFirstWait - std::chrono::milliseconds(5)) {
        transport.pump();
        early_attempts += node.reprepare_calls - 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(early_attempts == 0);
    (void)attempt(3);
    CHECK(transport.status_snapshot().reacquire_attempts == 3);
}

TEST_CASE("GpuAudioTransport re-acquire that delivers again clears the stuck-late state",
          "[gpu_audio][transport][status][health][reacquire]") {
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
    node.claims_provider = true;
    node.expose_health = true;
    node.expose_reprepare = true;
    node.health.gpu_kernel_available = true;
    REQUIRE(node.prepare());
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {.ring_blocks = 8,
                                      .recovery_policy = GpuAudioRecoveryPolicy::AutoReacquire,
                                      .reacquire_backoff_ns = 0}));
    Block input(1, 32), output(1, 32);
    input.fill(0.25f);
    const auto run = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            auto in = input.cview();
            auto out = output.view();
            transport.process(in, out, 32);
        }
    };
    run(40);
    CHECK(validated(transport.status_snapshot()).provider_state == GpuAudioProviderState::Ready);
    // Stuck late, then the provider is lost.
    node.realtime_status = detail::kRealtimeGpuMissed;
    run(40);
    auto status = validated(transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    node.inject_recovery(GpuAudioRecoveryReason::ProviderLost, node.next_sequence);
    transport.pump();
    REQUIRE(node.reprepare_calls == 1);
    // The window describes the replaced provider no longer: a few GPU blocks
    // from the new epoch read Ready, though most of the last 32 were not GPU.
    run(4);
    transport.pump();
    status = validated(transport.status_snapshot());
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderReacquired);
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.selected_engine == GpuAudioEngine::Gpu);
    transport.release();
}

TEST_CASE("GpuAudioTransport status reports a lost provider as Unavailable after a fence",
          "[gpu_audio][transport][status][health]") {
    HealthHarness h;

    // Positive control: the healthy authenticated stand-in is Ready, and the
    // health sampled at prepare() is already visible.
    auto status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.selected_engine == GpuAudioEngine::Gpu);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(status.recovery_sequence == UINT64_MAX);
    CHECK(status.epoch == 1);
    CHECK(status.delivery_epoch_active);

    // Within the 3 blocks the hook reports produced per pump, as the
    // validator requires GPU deliveries never to exceed produced blocks.
    h.node.health.terminal_counts[slot(GpuAudioTerminalDisposition::GpuDelivered)] = 3;
    h.callback();
    h.transport.pump();
    status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.terminal_counts[slot(GpuAudioTerminalDisposition::GpuDelivered)] == 3);

    h.node.inject_recovery(GpuAudioRecoveryReason::ProviderLost, 7);
    h.node.health.terminal_counts[slot(GpuAudioTerminalDisposition::DeviceLost)] = 1;
    // The worker samples health after its service step; until then the
    // snapshot is the previous coherent copy, never a half-applied one.
    CHECK(validated(h.transport.status_snapshot()).provider_state == GpuAudioProviderState::Ready);
    h.transport.pump();

    status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(status.recovery_sequence == 7);
    CHECK(status.epoch == 1);
    CHECK_FALSE(status.delivery_epoch_active);
    CHECK(status.terminal_counts[slot(GpuAudioTerminalDisposition::DeviceLost)] == 1);
    // The provider identity is unchanged, but the GPU no longer delivers: the
    // engine names the CPU fallback that produces the audio.
    CHECK(status.provider == GpuAudioProvider::Dawn);
    CHECK(status.selected_engine == GpuAudioEngine::Cpu);

    h.transport.release();
    status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Uninitialized);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
}

TEST_CASE("GpuAudioTransport status maps each recovery cause to Degraded or Unavailable",
          "[gpu_audio][transport][status][health]") {
    struct Case {
        GpuAudioRecoveryReason reason;
        bool device_lost;
        GpuAudioProviderState expected;
    };
    const std::array<Case, 12> cases{{
        {GpuAudioRecoveryReason::SequenceGap, false, GpuAudioProviderState::Degraded},
        {GpuAudioRecoveryReason::InputSaturated, false, GpuAudioProviderState::Degraded},
        {GpuAudioRecoveryReason::ProviderFailure, false, GpuAudioProviderState::Degraded},
        {GpuAudioRecoveryReason::OfflineFence, false, GpuAudioProviderState::Degraded},
        {GpuAudioRecoveryReason::InvalidCallback, false, GpuAudioProviderState::Degraded},
        // A slow but alive GPU: the breaker trips and the fallback carries.
        {GpuAudioRecoveryReason::SustainedDeadlineMisses, false, GpuAudioProviderState::Degraded},
        {GpuAudioRecoveryReason::ProviderLost, false, GpuAudioProviderState::Unavailable},
        {GpuAudioRecoveryReason::ProviderUnresponsive, false, GpuAudioProviderState::Unavailable},
        // A loss observed after a failure it caused is reported as the loss.
        {GpuAudioRecoveryReason::ProviderFailure, true, GpuAudioProviderState::Unavailable},
        // An independent cause stays first: the device was not what failed.
        {GpuAudioRecoveryReason::SequenceGap, true, GpuAudioProviderState::Unavailable},
        {GpuAudioRecoveryReason::None, true, GpuAudioProviderState::Unavailable},
        // Sustained misses describe a live GPU; a later loss is the loss.
        {GpuAudioRecoveryReason::SustainedDeadlineMisses, true, GpuAudioProviderState::Unavailable},
    }};
    for (const auto& c : cases) {
        HealthHarness h;
        if (c.reason != GpuAudioRecoveryReason::None)
            h.node.inject_recovery(c.reason, 11);
        h.node.health.device_lost = c.device_lost;
        h.transport.pump();
        const auto status = validated(h.transport.status_snapshot());
        INFO("reason=" << static_cast<int>(c.reason) << " device_lost=" << c.device_lost);
        CHECK(status.provider_state == c.expected);
        // A loss outranks the provider failures and stalls it induces.
        const bool loss_outranks =
            c.device_lost && (c.reason == GpuAudioRecoveryReason::SustainedDeadlineMisses ||
                              c.reason == GpuAudioRecoveryReason::ProviderFailure ||
                              c.reason == GpuAudioRecoveryReason::ProviderUnresponsive);
        CHECK(status.recovery_reason ==
              (loss_outranks ? GpuAudioRecoveryReason::ProviderLost : c.reason));
        CHECK(status.selected_engine == GpuAudioEngine::Cpu);
    }

    // A fenced node with no latched cause (for example between epochs) is
    // never reported Ready.
    HealthHarness h;
    h.node.health.fenced = true;
    h.transport.pump();
    CHECK(validated(h.transport.status_snapshot()).provider_state ==
          GpuAudioProviderState::Degraded);
}

TEST_CASE("GpuAudioTransport status without a node health hook cannot see a fence",
          "[gpu_audio][transport][status][health]") {
    // Planted negative: the same fenced node, hosted without its health hook,
    // is indistinguishable from a healthy one. The health read is what makes
    // the status truthful. With no hook there is no kernel witness either, so
    // the GPU claim it projects fails validation rather than passing as Ready.
    HealthHarness h(true, false);
    h.node.inject_recovery(GpuAudioRecoveryReason::ProviderLost, 3);
    h.transport.pump();
    const auto status = h.transport.status_snapshot();
    CHECK(validate_gpu_audio_status(status).error == GpuAudioStatusError::GpuEngineWithoutKernel);
    CHECK(status.provider_state == GpuAudioProviderState::Ready);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
    CHECK(h.node.health_calls == 0);
}

TEST_CASE("GpuAudioTransport status never reports Ready without a health sample",
          "[gpu_audio][transport][status][health]") {
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
    node.claims_provider = true;
    node.expose_health = true;
    node.health_read_ok = false;
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {8}));
    t.pump();
    CHECK(node.health_calls >= 2);
    const auto status = validated(t.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);
}

TEST_CASE("GpuAudioTransport status reflects the offline fence without a worker pump",
          "[gpu_audio][transport][status][health]") {
    HealthHarness h;
    // The test node's fence latches OfflineFence the way a stamped bridge does.
    h.node.fence_result = true;
    auto in = h.input.cview();
    auto out = h.output.view();
    const auto calls_before = h.node.health_calls;
    h.node.inject_recovery(GpuAudioRecoveryReason::OfflineFence, 0);
    h.transport.process_offline(in, out, 32);
    CHECK(h.node.fence_calls == 1);
    CHECK(h.node.health_calls == calls_before + 1);
    const auto status = validated(h.transport.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::OfflineFence);
}

TEST_CASE("GpuAudioTransport health sampling and status reads do not allocate",
          "[gpu_audio][transport][status][health][rt]") {
    HealthHarness h;
    h.node.inject_recovery(GpuAudioRecoveryReason::ProviderLost, 9);
    std::size_t allocations = 0;
    GpuAudioStatus status;
    {
        pulp::test::RtAllocationProbe probe;
        h.callback();
        h.transport.pump();
        status = h.transport.status_snapshot();
        allocations = probe.allocation_count();
    }
    // Validate outside the probe window: Catch's message streams allocate on
    // first use, which is not the code under measurement.
    (void)validated(status);
    CHECK(allocations == 0);
    CHECK(status.provider_state == GpuAudioProviderState::Unavailable);

    // Positive control: the probe sees an allocation in the same scope shape.
    {
        pulp::test::RtAllocationProbe probe;
        // The volatile store keeps the compiler from eliding the allocation.
        static std::uint64_t* volatile sink = nullptr;
        sink = new std::uint64_t(1);
        delete sink;
        CHECK(probe.saw_allocation());
    }
}

TEST_CASE("GpuAudioTransport status reads stay coherent while the worker republishes health",
          "[gpu_audio][transport][status][health][concurrency]") {
    HealthHarness h;
    std::atomic<bool> reader_started{false};
    std::atomic<bool> writing{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> sampled_reads_during_writes{0};
    std::atomic<std::uint64_t> invalid{0};
    std::thread reader([&] {
        reader_started.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + pulp::test::kProgressDeadline;
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            const bool during_writes = writing.load(std::memory_order_acquire);
            const auto status = h.transport.status_snapshot();
            if (!validate_gpu_audio_status(status).accepted())
                invalid.fetch_add(1, std::memory_order_relaxed);
            // Unsampled reads keep the default sequence; they prove nothing.
            if (status.recovery_sequence == UINT64_MAX)
                continue;
            // Every published sample keeps epoch, sequence and the terminal
            // count equal. Any mix of two samples breaks that invariant.
            const auto lost = status.terminal_counts[slot(GpuAudioTerminalDisposition::DeviceLost)];
            if (status.epoch != status.recovery_sequence || lost != status.recovery_sequence)
                torn.fetch_add(1, std::memory_order_relaxed);
            if (during_writes && writing.load(std::memory_order_acquire))
                sampled_reads_during_writes.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // Start barrier: the reader is running before the first write.
    REQUIRE(pulp::test::wait_for_condition(
        [&] { return reader_started.load(std::memory_order_acquire); }));
    writing.store(true, std::memory_order_release);
    for (std::uint64_t i = 1; i <= 20000 || sampled_reads_during_writes.load() == 0; ++i) {
        h.node.health.recovery_reason = (i & 1U) != 0U ? GpuAudioRecoveryReason::ProviderLost
                                                       : GpuAudioRecoveryReason::SequenceGap;
        h.node.health.recovery_sequence = i;
        h.node.health.epoch = i;
        h.node.health.terminal_counts[slot(GpuAudioTerminalDisposition::DeviceLost)] = i;
        h.transport.pump();
        if (i > 2'000'000)
            break; // bounded: the CHECK below reports a reader that never overlapped
    }
    writing.store(false, std::memory_order_release);
    done.store(true, std::memory_order_release);
    reader.join();
    // The reads that count were taken while writes were in flight.
    CHECK(sampled_reads_during_writes.load() > 0);
    CHECK(torn.load() == 0);
    CHECK(invalid.load() == 0);
    const auto status = validated(h.transport.status_snapshot());
    CHECK(status.recovery_sequence >= 20000);
    CHECK(status.epoch == status.recovery_sequence);
}

namespace {

class StatusReportingNode final : public GpuAudioNode {
  public:
    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "status-reporting";
        d.input_channels = 1;
        d.output_channels = 1;
        d.block_size = 32;
        d.sample_rate = 48000;
        d.latency_blocks = 2;
        return d;
    }
    bool prepare() override {
        return true;
    }
    void process_block(const BufferView<const float>&, BufferView<float>& out, uint32_t) override {
        out.clear();
    }
    GpuAudioStatus status_snapshot() const noexcept override {
        return reported;
    }
    GpuAudioStatus reported;
};

} // namespace

TEST_CASE("GpuAudioTransport status folds in a staged node's own status",
          "[gpu_audio][transport][status][health]") {
    StatusReportingNode node;
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {8}));

    // Base default (Uninitialized) carries no health: the staged status holds.
    auto status = validated(t.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Degraded);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::None);

    node.reported.provider_state = GpuAudioProviderState::Unavailable;
    node.reported.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
    node.reported.recovery_sequence = 4;
    // The worker samples the staged node's own status after each pump.
    CHECK(validated(t.status_snapshot()).provider_state == GpuAudioProviderState::Degraded);
    t.pump();
    status = validated(t.status_snapshot());
    CHECK(status.provider_state == GpuAudioProviderState::Unavailable);
    CHECK(status.recovery_reason == GpuAudioRecoveryReason::ProviderLost);
    CHECK(status.recovery_sequence == 4);

    // A node's own Ready never upgrades the unauthenticated staged path.
    node.reported = {};
    node.reported.provider_state = GpuAudioProviderState::Ready;
    t.pump();
    CHECK(validated(t.status_snapshot()).provider_state == GpuAudioProviderState::Degraded);
    CHECK(validated(t.status_snapshot()).recovery_reason == GpuAudioRecoveryReason::None);
}

TEST_CASE("Least-healthy lane selection prefers the worse state, then a latched cause",
          "[gpu_audio][transport][status][health]") {
    GpuAudioStatus ready;
    ready.provider_state = GpuAudioProviderState::Ready;
    GpuAudioStatus degraded;
    degraded.provider_state = GpuAudioProviderState::Degraded;
    GpuAudioStatus degraded_gap = degraded;
    degraded_gap.recovery_reason = GpuAudioRecoveryReason::SequenceGap;
    GpuAudioStatus lost;
    lost.provider_state = GpuAudioProviderState::Unavailable;
    lost.recovery_reason = GpuAudioRecoveryReason::ProviderLost;

    const auto pick = [](std::initializer_list<const GpuAudioStatus*> lanes) {
        return detail::least_healthy_status(lanes.begin(), lanes.size());
    };
    CHECK(pick({}) == nullptr);
    CHECK(pick({nullptr, &ready}) == &ready);
    CHECK(pick({&ready, &lost}) == &lost);
    CHECK(pick({&lost, &degraded_gap}) == &lost);
    CHECK(pick({&degraded, &degraded_gap}) == &degraded_gap);
    // Ties keep the first lane.
    GpuAudioStatus degraded_too = degraded;
    CHECK(pick({&degraded, &degraded_too}) == &degraded);
}

TEST_CASE("Least-healthy lane selection ranks an unprepared lane worst",
          "[gpu_audio][transport][status][health]") {
    GpuAudioStatus ready;
    ready.provider_state = GpuAudioProviderState::Ready;
    GpuAudioStatus lost;
    lost.provider_state = GpuAudioProviderState::Unavailable;
    lost.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
    GpuAudioStatus unprepared;
    const std::array<const GpuAudioStatus*, 2> ready_then_unprepared{&ready, &unprepared};
    CHECK(detail::least_healthy_status(ready_then_unprepared.data(), 2) == &unprepared);
    const std::array<const GpuAudioStatus*, 2> lost_then_unprepared{&lost, &unprepared};
    CHECK(detail::least_healthy_status(lost_then_unprepared.data(), 2) == &unprepared);
}

TEST_CASE("GpuAudioTransport status maps a loss after an earlier cause from the node status",
          "[gpu_audio][transport][status][health]") {
    // A node that latched SequenceGap and then lost its provider keeps the
    // first cause but reports Unavailable; the projection must keep both.
    GpuAudioStatus status;
    status.provider_state = GpuAudioProviderState::Unavailable;
    status.recovery_reason = GpuAudioRecoveryReason::SequenceGap;
    auto health = detail::health_from_status(status);
    CHECK(health.device_lost);
    CHECK(detail::provider_state_for_health(health) == GpuAudioProviderState::Unavailable);
    CHECK(health.recovery_reason == GpuAudioRecoveryReason::SequenceGap);

    // Positive control: the same cause without a loss stays Degraded.
    status.provider_state = GpuAudioProviderState::Degraded;
    health = detail::health_from_status(status);
    CHECK_FALSE(health.device_lost);
    CHECK(detail::provider_state_for_health(health) == GpuAudioProviderState::Degraded);

    // A latched loss reason is loss even when the node reports Degraded.
    status.recovery_reason = GpuAudioRecoveryReason::ProviderUnresponsive;
    CHECK(detail::health_from_status(status).device_lost);
}

namespace {

// Staged node whose status_snapshot() records the calling thread and fails a
// sentinel once destroyed, so a reader that reaches into it is visible.
class SentinelNode final : public GpuAudioNode {
  public:
    explicit SentinelNode(std::atomic<std::uint32_t>& reader_calls, std::thread::id& reader)
        : reader_calls_(reader_calls), reader_(reader) {}
    ~SentinelNode() override {
        alive_ = 0;
    }
    GpuAudioNodeDescriptor descriptor() const override {
        GpuAudioNodeDescriptor d;
        d.name = "sentinel";
        d.input_channels = 1;
        d.output_channels = 1;
        d.block_size = 32;
        d.sample_rate = 48000;
        d.latency_blocks = 2;
        return d;
    }
    bool prepare() override {
        ++prepares;
        return true;
    }
    void process_block(const BufferView<const float>&, BufferView<float>& out, uint32_t) override {
        out.clear();
    }
    GpuAudioStatus status_snapshot() const noexcept override {
        if (std::this_thread::get_id() == reader_ || alive_ != kAlive)
            reader_calls_.fetch_add(1, std::memory_order_relaxed);
        GpuAudioStatus status;
        status.provider_state = GpuAudioProviderState::Unavailable;
        status.recovery_reason = GpuAudioRecoveryReason::ProviderLost;
        status.recovery_sequence = 0;
        status.sample_rate = 48000;
        status.block_size = 32;
        return status;
    }
    std::uint32_t prepares = 0;

  private:
    static constexpr std::uint32_t kAlive = 0xa11fe;
    std::atomic<std::uint32_t>& reader_calls_;
    std::thread::id& reader_;
    volatile std::uint32_t alive_ = kAlive;
};

} // namespace

TEST_CASE("GpuAudioTransport status never touches the node while it is re-prepared or destroyed",
          "[gpu_audio][transport][status][health][concurrency]") {
    std::atomic<std::uint32_t> node_calls_from_reader{0};
    std::thread::id reader_id{};
    GpuAudioTransport t;
    auto node = std::make_unique<SentinelNode>(node_calls_from_reader, reader_id);
    REQUIRE(t.prepare(node.get(), {8}));

    std::atomic<bool> reader_started{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> unavailable_reads{0};
    std::atomic<std::uint64_t> invalid{0};
    std::thread reader([&] {
        reader_id = std::this_thread::get_id();
        reader_started.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + pulp::test::kProgressDeadline;
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            const auto status = t.status_snapshot();
            (void)t.capability_report();
            if (!validate_gpu_audio_status(status).accepted())
                invalid.fetch_add(1, std::memory_order_relaxed);
            if (status.provider_state == GpuAudioProviderState::Unavailable &&
                status.recovery_reason == GpuAudioRecoveryReason::ProviderLost)
                unavailable_reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    REQUIRE(pulp::test::wait_for_condition(
        [&] { return reader_started.load(std::memory_order_acquire); }));
    REQUIRE(pulp::test::wait_for_condition(
        [&] { return unavailable_reads.load(std::memory_order_relaxed) > 0; }));

    // The host's usual sequence re-prepares the node under a prepared
    // transport, then destroys it before the transport.
    for (int i = 0; i < 2000; ++i)
        REQUIRE(node->prepare());
    node.reset();
    for (int i = 0; i < 2000; ++i)
        (void)t.status_snapshot();
    t.release();
    done.store(true, std::memory_order_release);
    reader.join();

    CHECK(node_calls_from_reader.load() == 0);
    CHECK(unavailable_reads.load() > 0);
    CHECK(invalid.load() == 0);
    CHECK(validated(t.status_snapshot()).provider_state == GpuAudioProviderState::Uninitialized);
}

TEST_CASE("GpuAudioTransport status never calls the node health hook from a reader",
          "[gpu_audio][transport][status][health][concurrency]") {
    HealthHarness h;
    h.node.inject_recovery(GpuAudioRecoveryReason::ProviderLost, 2);
    h.transport.pump();
    std::thread reader([&] {
        h.node.forbidden_health_thread = std::this_thread::get_id();
        for (int i = 0; i < 1000; ++i)
            (void)h.transport.status_snapshot();
    });
    reader.join();
    CHECK(h.node.health_calls_from_forbidden_thread.load() == 0);
    // Positive control: the same detector fires when the hook is called on
    // the forbidden thread.
    h.node.forbidden_health_thread = std::this_thread::get_id();
    h.transport.pump();
    CHECK(h.node.health_calls_from_forbidden_thread.load() == 1);
}

TEST_CASE("GpuAudioTransport status names the engine that delivers after a fence",
          "[gpu_audio][transport][status][health]") {
    for (const auto policy : {MissPolicy::CpuFallback, MissPolicy::Silence}) {
        RealtimeHookNode node(1, 32, policy);
        node.claims_provider = true;
        node.expose_health = true;
        node.health.gpu_kernel_available = true;
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        REQUIRE(t.prepare(&node, {8}));
        INFO("policy=" << static_cast<int>(policy));
        // Positive control: an unfenced authenticated provider delivers GPU blocks.
        CHECK(validated(t.status_snapshot()).selected_engine == GpuAudioEngine::Gpu);

        node.inject_recovery(GpuAudioRecoveryReason::SequenceGap, 1);
        t.pump();
        const auto status = validated(t.status_snapshot());
        CHECK(status.provider_state == GpuAudioProviderState::Degraded);
        // Degraded never names the GPU: the fallback (or silence) is delivering.
        CHECK(status.selected_engine ==
              (policy == MissPolicy::CpuFallback ? GpuAudioEngine::Cpu : GpuAudioEngine::Unknown));
    }
}

TEST_CASE("GpuAudioTransport status carries the node's provider facts",
          "[gpu_audio][transport][status][health]") {
    // Realtime node: the facts arrive through the health hook. The node does
    // not claim a provider, so a kernel bit left unset is still a valid status.
    HealthHarness h(false);
    // Positive control: a node that reports nothing leaves the defaults.
    auto status = validated(h.transport.status_snapshot());
    CHECK_FALSE(status.gpu_kernel_available);
    CHECK(status.completion_mode == GpuAudioCompletionMode::Unknown);
    CHECK_FALSE(status.timestamps_available);
    CHECK(status.last_delivery_ns == 0);

    h.node.health.gpu_kernel_available = true;
    h.node.health.completion_mode = GpuAudioCompletionMode::TimedWaitAny;
    h.node.health.timestamps_available = true;
    h.node.health.last_delivery_ns = 1234;
    h.transport.pump();
    status = validated(h.transport.status_snapshot());
    CHECK(status.gpu_kernel_available);
    CHECK(status.completion_mode == GpuAudioCompletionMode::TimedWaitAny);
    CHECK(status.timestamps_available);
    CHECK(status.last_delivery_ns == 1234);

    // Staged node: the facts arrive through its own status projection.
    StatusReportingNode node;
    GpuAudioTransport t;
    REQUIRE(t.prepare(&node, {8}));
    node.reported.provider_state = GpuAudioProviderState::Degraded;
    node.reported.gpu_kernel_available = true;
    node.reported.completion_mode = GpuAudioCompletionMode::ProcessEvents;
    t.pump();
    status = validated(t.status_snapshot());
    CHECK(status.gpu_kernel_available);
    CHECK(status.completion_mode == GpuAudioCompletionMode::ProcessEvents);
}

TEST_CASE("GpuAudioTransport releases a pipelining node it outlived through the detached link",
          "[gpu_audio][transport][registry][pipeline]") {
    std::shared_ptr<detail::WorkerPipelineLink> link;
    GpuAudioTransport transport;
    {
        auto node = std::make_unique<PipelineGainNode>(1, 32);
        REQUIRE(node->prepare());
        REQUIRE(transport.prepare(node.get(), {8}));
        CHECK(node->hosted);
        link = detail::realtime_gpu_node_binding(node.get()).pipeline;
        REQUIRE(link != nullptr);
        CHECK(link->attached());
        // Positive control: releasing while the node lives undoes the opt-in.
        transport.release();
        CHECK_FALSE(node->hosted);
        REQUIRE(transport.prepare(node.get(), {8}));
        CHECK(node->hosted);
    }
    // The host destroyed the node first; release() must reach only the link.
    CHECK_FALSE(link->attached());
    CHECK_FALSE(link->set_hosting(false));
    transport.release();
    CHECK_FALSE(transport.capability_report().prepared);
}

TEST_CASE("GpuAudioTransport binds a node it never names through the registry hook",
          "[gpu_audio][transport][registry]") {
    // The transport links the real registry; this node is unknown to it and
    // registers only by overriding bind_realtime_path().
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback, 3);
    node.claims_provider = true;
    REQUIRE(node.prepare());
    CHECK(detail::realtime_gpu_node_path(&node).active());
    CHECK_FALSE(detail::requires_realtime_gpu_path(&node));
    CHECK(detail::realtime_gpu_provider(&node) == GpuAudioProvider::Dawn);
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    CHECK(transport.capability_report().provider == GpuAudioProvider::Dawn);
    transport.release();

    // A CPU provider needs no authentication to be reported as CPU.
    node.claims_provider = false;
    node.identity_provider = GpuAudioProvider::Cpu;
    CHECK(detail::realtime_gpu_provider(&node) == GpuAudioProvider::Cpu);

    // A staged node does not override the hook, so it binds nothing.
    GainNode staged(1, 32, 1.0f, MissPolicy::CpuFallback);
    CHECK_FALSE(detail::realtime_gpu_node_path(&staged).active());
    CHECK_FALSE(detail::requires_realtime_gpu_path(&staged));
    CHECK(detail::realtime_gpu_provider(&staged) == GpuAudioProvider::Unknown);
}

TEST_CASE("GpuAudioTransport reports no GPU provider for an unauthenticated identity",
          "[gpu_audio][transport][registry]") {
    // Planted negative: the identity names Dawn but does not authenticate,
    // so it is not a GPU provider claim.
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback, 3);
    node.claims_provider = false;
    node.identity_provider = GpuAudioProvider::Dawn;
    REQUIRE(node.prepare());
    CHECK(detail::realtime_gpu_provider(&node) == GpuAudioProvider::Unknown);
    GpuAudioTransport transport;
    REQUIRE(transport.prepare(&node, {8}));
    CHECK(transport.capability_report().provider == GpuAudioProvider::Unknown);
    transport.release();
}
namespace {

std::uint64_t steady_now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct WakeLatencyResult {
    std::uint64_t p50_ns = 0;
    std::uint64_t p99_ns = 0;
    std::size_t samples = 0;
};

// Worker wake latency: a completion becomes ready mid-block (between callback
// posts) and the transport worker must notice it in a service step. Callbacks
// run at the block period on their own thread, as an audio device would.
WakeLatencyResult measure_worker_wake_latency(bool notify_on_completion) {
    constexpr std::uint32_t kBlock = 128;
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr int kCompletions = 400;
    RealtimeHookNode node(1, kBlock, MissPolicy::CpuFallback);
    node.service_return_blocks = 0;
    node.expose_notifier = notify_on_completion;
    node.wake_latencies_ns.reserve(kCompletions);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    GpuAudioTransport::Config config;
    config.ring_blocks = 8;
    config.run_worker_thread = true;
    config.wake_on_write = true;
    REQUIRE(t.prepare(&node, config));
    std::atomic<bool> running{true};
    const auto period = std::chrono::nanoseconds(1'000'000'000ull * kBlock / kSampleRate);
    std::thread callback([&] {
        Block input(1, kBlock), output(1, kBlock);
        auto next = std::chrono::steady_clock::now();
        while (running.load(std::memory_order_acquire)) {
            auto in = input.cview();
            auto out = output.view();
            t.process(in, out, kBlock);
            next += period;
            std::this_thread::sleep_until(next);
        }
    });
    for (int i = 0; i < kCompletions; ++i) {
        // Land between callback posts: a fixed fraction of the period after
        // an arbitrary point, so the poll phase is sampled uniformly.
        std::this_thread::sleep_for(period * (1 + (i % 7)) / 5);
        node.completion_ready_ns.store(steady_now_ns(), std::memory_order_release);
        if (notify_on_completion)
            node.notify_completion();
        (void)pulp::test::wait_for_condition(
            [&] { return node.completion_ready_ns.load(std::memory_order_acquire) == 0; });
    }
    running.store(false, std::memory_order_release);
    callback.join();
    t.release();
    auto samples = node.wake_latencies_ns;
    std::sort(samples.begin(), samples.end());
    WakeLatencyResult result;
    result.samples = samples.size();
    if (!samples.empty()) {
        result.p50_ns = samples[samples.size() / 2];
        result.p99_ns = samples[std::min(samples.size() - 1, samples.size() * 99 / 100)];
    }
    return result;
}

} // namespace

TEST_CASE("GpuAudioTransport worker wake latency benchmark",
          "[.][benchmark][gpu_audio][transport]") {
    for (const bool notify : {false, true}) {
        const auto r = measure_worker_wake_latency(notify);
        WARN("worker wake latency, completion notifier "
             << (notify ? "on" : "off") << ": p50 " << r.p50_ns / 1000.0 << " us, p99 "
             << r.p99_ns / 1000.0 << " us over " << r.samples << " completions");
        CHECK(r.samples > 0);
    }
}

#if !defined(_WIN32)
TEST_CASE("GpuAudioTransport idle worker CPU benchmark", "[.][benchmark][gpu_audio][transport]") {
    for (const bool notifier : {false, true}) {
        // No callbacks and no completions: whatever CPU the process uses over the
        // window is the worker's idle wake cost.
        constexpr std::uint32_t kBlock = 128;
        constexpr auto kWindow = std::chrono::seconds(2);
        RealtimeHookNode node(1, kBlock, MissPolicy::CpuFallback);
        node.service_return_blocks = 0;
        node.expose_notifier = notifier;
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        GpuAudioTransport::Config config;
        config.ring_blocks = 8;
        config.run_worker_thread = true;
        config.wake_on_write = true;
        REQUIRE(t.prepare(&node, config));
        const auto cpu_us = [] {
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1e6 +
                   static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
        };
        const std::uint32_t services_before = node.service_calls.load();
        const auto cpu_before = cpu_us();
        std::this_thread::sleep_for(kWindow);
        const auto cpu_after = cpu_us();
        const auto services = node.service_calls - services_before;
        t.release();
        const double block_periods = 2.0 * 48000.0 / kBlock;
        WARN("idle worker, completion notifier "
             << (notifier ? "on" : "off") << ": " << services << " service wakes, "
             << (cpu_after - cpu_before) / block_periods << " us CPU per block period over 2 s");
        CHECK(services > 0);
    }
}
#endif

TEST_CASE("GpuAudioTransport installs the completion notifier only for a waking owned worker",
          "[gpu_audio][transport][wake]") {
    struct Case {
        bool run_worker;
        bool wake_on_write;
        bool supported;
        std::uint32_t expected_installs;
    };
    for (const auto c : {Case{true, true, true, 1}, Case{true, false, true, 0},
                         Case{false, true, true, 0}, Case{true, true, false, 0}}) {
        RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
        node.expose_notifier = true;
        node.notifier_supported = c.supported;
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        GpuAudioTransport::Config config;
        config.ring_blocks = 8;
        config.run_worker_thread = c.run_worker;
        config.wake_on_write = c.wake_on_write;
        REQUIRE(t.prepare(&node, config));
        INFO("worker=" << c.run_worker << " wake=" << c.wake_on_write
                       << " supported=" << c.supported);
        CHECK(node.notifier_installs == c.expected_installs);
        CHECK((node.installed_notify.load() != nullptr) == (c.expected_installs == 1));
        t.release();
        // Cleared after the worker joined, so no later wake reaches a released
        // transport.
        CHECK(node.notifier_clears == c.expected_installs);
        CHECK(node.installed_notify.load() == nullptr);
    }
}

TEST_CASE("GpuAudioTransport keeps wake-on-write opt-in so the default callback makes no syscall",
          "[gpu_audio][transport][wake]") {
    CHECK_FALSE(GpuAudioTransport::Config{}.wake_on_write);
}

// The callback under the RT probe: on UNIX it aborts the binary on any
// allocation or pthread mutex/rwlock lock inside the scope; elsewhere it
// counts allocations. Every callback position runs: priming, ready output,
// misses with and without resync debt, dropped input, rejected views, and the
// realtime-hook path's ready, priming and missed results. The probe cannot
// see a semaphore post: it interposes allocation and pthread locks only, and
// wake-on-write's post is dispatch_semaphore_signal (a Mach semaphore_signal
// trap while the worker is parked) or a futex wake, neither of which takes a
// pthread lock. The default config leaves wake-on-write off, so no post runs
// here.
TEST_CASE("GpuAudioTransport callback takes no lock and allocates nothing on the default config",
          "[gpu_audio][transport][realtime]") {
    constexpr uint32_t BS = 32;
    Block in(1, BS), out(1, BS), short_out(1, BS / 2);
    in.fill(1.0f);
    auto iv = in.cview();
    auto ov = out.view();
    auto short_ov = short_out.view();

    SECTION("staged path") {
        GainNode node(1, BS, 2.0f, MissPolicy::CpuFallback);
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        REQUIRE(t.prepare(&node, {}));
        // The worker keeps pace, stalls long enough for the input ring to
        // fill, then catches up; each pump runs outside the probe.
        std::size_t allocations = 0;
        for (int k = 0; k < 24; ++k) {
            {
                pulp::test::ScopedRtProcessProbe probe;
                t.process(iv, k == 2 ? short_ov : ov, BS); // one rejected view
                allocations += probe.allocation_count();
            }
            if (k < 4 || k >= 12)
                t.pump();
        }
        CHECK(allocations == 0);
        CHECK(t.stats().input_dropped_frames > 0);
        CHECK(t.stats().resynced_blocks > 0);
        CHECK(t.delivery_snapshot().worker_output_blocks > 0);
        CHECK(t.delivery_snapshot().cpu_fallback_blocks > 0);
        CHECK(t.delivery_snapshot().invalid_blocks == 1);
    }
    SECTION("staged path with its owned polling worker running") {
        GainNode node(1, BS, 2.0f, MissPolicy::CpuFallback);
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        GpuAudioTransport::Config config;
        config.run_worker_thread = true;
        REQUIRE(t.prepare(&node, config));
        std::size_t allocations = 1;
        {
            pulp::test::ScopedRtProcessProbe probe;
            for (int k = 0; k < 64; ++k) {
                t.process(iv, ov, BS);
                const auto until =
                    std::chrono::steady_clock::now() + std::chrono::microseconds(200);
                while (std::chrono::steady_clock::now() < until) {
                }
            }
            allocations = probe.allocation_count();
        }
        CHECK(allocations == 0);
        t.release();
        CHECK(t.stats().produced_blocks > 0);
    }
    SECTION("realtime-hook path") {
        RealtimeHookNode node(1, BS, MissPolicy::CpuFallback);
        node.stateful_fallback = true;
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        REQUIRE(t.prepare(&node, {}));
        std::size_t allocations = 1;
        {
            pulp::test::ScopedRtProcessProbe probe;
            for (const auto status : {detail::kRealtimeGpuPriming, detail::kRealtimeGpuReady,
                                      detail::kRealtimeGpuMissed, detail::kRealtimeGpuInactive}) {
                node.realtime_status = status;
                t.process(iv, ov, BS);
            }
            t.process(iv, short_ov, BS); // rejected view
            allocations = probe.allocation_count();
        }
        CHECK(allocations == 0);
        CHECK(node.realtime_calls == 5);
        CHECK(node.cpu_fallback_calls >= 2);
    }
}

#if PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS
TEST_CASE("GpuAudioTransport callback probe traps a planted blocking lock",
          "[gpu_audio][transport][realtime][negative]") {
    // Positive control for the probe above: the same scope in this binary
    // aborts on a pthread lock.
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // Resolve the interposed symbol before entering the scope, so the
        // child dies from the planted lock and not from loader work.
        (void)::pthread_mutex_lock(&mutex);
        (void)::pthread_mutex_unlock(&mutex);
        pulp::test::ScopedRtProcessProbe probe;
        (void)::pthread_mutex_lock(&mutex);
        _exit(EXIT_FAILURE);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK((WTERMSIG(status) == SIGABRT || WTERMSIG(status) == SIGTRAP));
    REQUIRE(::pthread_mutex_destroy(&mutex) == 0);
}
#endif

TEST_CASE("GpuAudioTransport completion wakes never count as progress",
          "[gpu_audio][transport][wake]") {
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
    node.expose_notifier = true;
    node.expose_health = true;
    node.service_return_blocks = 0; // the provider completes nothing
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    GpuAudioTransport::Config config;
    config.ring_blocks = 8;
    config.run_worker_thread = true;
    config.wake_on_write = true;
    REQUIRE(t.prepare(&node, config));
    REQUIRE(node.notifier_installs == 1);
    const std::uint32_t services_before = node.service_calls.load();
    for (int i = 0; i < 1000; ++i)
        node.notify_completion();
    // The wakes do run service steps...
    CHECK(pulp::test::wait_for_condition([&] { return node.service_calls > services_before + 1; }));
    t.release();
    // ...but a wake with no completion produces nothing: progress is only
    // what a service step consumed.
    CHECK(t.stats().produced_blocks == 0);
    const auto status = validated(t.status_snapshot());
    CHECK(status.produced_blocks == 0);
}

TEST_CASE("GpuAudioTransport coalesces completion wakes instead of spinning",
          "[gpu_audio][transport][wake]") {
    constexpr std::uint32_t kBlock = 128;
    constexpr int kPosts = 1000;
    RealtimeHookNode node(1, kBlock, MissPolicy::CpuFallback);
    node.expose_notifier = true;
    node.service_return_blocks = 0;
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    GpuAudioTransport::Config config;
    config.ring_blocks = 8;
    config.run_worker_thread = true;
    config.wake_on_write = true;
    REQUIRE(t.prepare(&node, config));
    REQUIRE(node.notifier_installs == 1);
    // With a completion notifier installed the worker's fallback wait is one
    // block (the transport truncates to whole microseconds).
    const auto fallback_interval =
        std::chrono::microseconds(static_cast<long>(1.0e6 * kBlock / 48000.0));

    // Park the worker inside a service step, then land every post while it is
    // held. No wait can consume a post in between, so all of them are pending
    // when the pump returns and must be coalesced.
    node.hold_service.store(true, std::memory_order_release);
    REQUIRE(pulp::test::wait_for_condition(
        [&] { return node.service_held.load(std::memory_order_acquire); }));
    const std::uint32_t services_before = node.service_calls.load();
    for (int i = 0; i < kPosts; ++i)
        node.notify_completion();
    const auto released_at = std::chrono::steady_clock::now();
    node.hold_service.store(false, std::memory_order_release);
    // The coalesced step must arrive; the extra time gives a spinning worker
    // room to show itself.
    CHECK(pulp::test::wait_for_condition([&] { return node.service_calls > services_before; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto services = node.service_calls.load() - services_before;
    const auto window = std::chrono::steady_clock::now() - released_at;
    t.release();

    // Coalesced, the pending posts cost one service step; every other step in
    // the window is the fallback timer. One spare step absorbs rounding at the
    // window edges. A worker that drains one post per pump spends a step per
    // post and lands near kPosts however slow the host is.
    const auto timer_steps = static_cast<std::uint32_t>(window / fallback_interval);
    const std::uint32_t bound = timer_steps + 2;
    const auto window_us = std::chrono::duration_cast<std::chrono::microseconds>(window).count();
    INFO("service steps after " << kPosts << " held posts: " << services << ", window " << window_us
                                << " us, bound " << bound);
    CHECK(services <= bound);
}

TEST_CASE("GpuAudioTransport survives completion events racing release",
          "[gpu_audio][transport][wake][concurrency]") {
    // A provider callback thread keeps notifying while the host releases the
    // transport. Release clears the notifier after the worker joins; after the
    // clear no notify may reach the transport.
    for (int round = 0; round < 20; ++round) {
        RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
        node.expose_notifier = true;
        node.service_return_blocks = 0;
        REQUIRE(node.prepare());
        GpuAudioTransport t;
        GpuAudioTransport::Config config;
        config.ring_blocks = 8;
        config.run_worker_thread = true;
        config.wake_on_write = true;
        REQUIRE(t.prepare(&node, config));
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> notifies{0};
        std::thread provider([&] {
            while (!stop.load(std::memory_order_acquire)) {
                node.notify_completion();
                notifies.fetch_add(1, std::memory_order_relaxed);
            }
        });
        REQUIRE(pulp::test::wait_for_condition(
            [&] { return notifies.load(std::memory_order_relaxed) > 100; }));
        t.release();
        CHECK(node.installed_notify.load() == nullptr);
        stop.store(true, std::memory_order_release);
        provider.join();
    }
}

#if defined(__APPLE__)
TEST_CASE("GpuAudioTransport runs its owned worker at interactive QoS",
          "[gpu_audio][transport][wake]") {
    RealtimeHookNode node(1, 32, MissPolicy::CpuFallback);
    REQUIRE(node.prepare());
    GpuAudioTransport t;
    GpuAudioTransport::Config config;
    config.ring_blocks = 8;
    config.run_worker_thread = true;
    REQUIRE(t.prepare(&node, config));
    CHECK(pulp::test::wait_for_condition([&] { return t.stats().worker_interactive_qos; }));
    t.release();
    CHECK_FALSE(t.stats().worker_interactive_qos);

    // Positive control: a caller-driven pump has no owned worker to promote.
    GpuAudioTransport manual;
    REQUIRE(manual.prepare(&node, {8}));
    manual.pump();
    CHECK_FALSE(manual.stats().worker_interactive_qos);
}
#endif
