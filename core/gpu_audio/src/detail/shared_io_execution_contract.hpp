#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

#include <pulp/gpu_audio/gpu_audio_node.hpp>

namespace pulp::gpu_audio::detail {

// This is deliberately private until the installed SDK contract is settled.
// `algorithmic_lead_blocks` is the number of complete blocks by which a result
// may trail the callback timeline. `pipeline_depth` is the logical bridge and
// completion-table capacity, so it must cover the declared lead plus the
// callback currently being published. `provider_slots` is the independent
// number of physical provider-arena slots; retired slots may be reused within
// the lead window. `max_inflight` is the maximum number of submitted blocks
// that the consumer intentionally keeps pending at once. It is independent
// from both capacity values so a receipt can distinguish a deeper provider
// arena from actual multi-flight dispatch.
enum class SharedIoPath : std::uint8_t { Cpu, StagedAsync, SharedHostPointer };
enum class SharedIoRequest : std::uint8_t {
    Auto,
    RequireSharedHostPointer,
    RequireStaged,
    RequireCpu,
};

enum class SharedIoFallbackReason : std::uint8_t {
    None,
    UnsupportedProvider,
    UnsupportedFeature,
    ProviderMismatch,
    AllocationFailed,
    DeviceLost,
    Timeout,
    SubmissionRejected,
    DeadlineExceeded,
    InputSaturated,
    SequenceGap,
    Teardown,
    CompletionFailed,
    InvalidCallback,
    PredictionRefused,
    CalibrationMissing,
    CalibrationStale,
};

enum class SharedIoContractError : std::uint8_t {
    None,
    InvalidShape,
    MissingAlgorithmicLead,
    MissingPipelineDepth,
    InsufficientPipelineDepth,
    CpuFallbackNotPrepared,
    ProviderUnavailable,
    RequestedPathUnavailable,
    MissingProviderSlots,
    InvalidMaxInflight,
    // Values outside the versioned backend-neutral enum domains are rejected
    // before path, fallback, or capacity relationships are evaluated.
    InvalidEnumValue,
};

// The callback lead consumes completion-table entries before a submitted
// block can be retired. Keep this arithmetic in one private policy seam so
// receipts and tests cannot accidentally treat physical slots as pipeline
// depth. This remains diagnostic-only; it does not enable a GPU path.
struct SharedIoDepthLeadPolicy {
    std::uint32_t lead_blocks = 0;
    std::uint32_t pipeline_depth = 0;
    std::uint32_t max_inflight = 1;

    constexpr std::uint32_t available_inflight() const noexcept {
        return pipeline_depth > lead_blocks ? pipeline_depth - lead_blocks : 0;
    }
    constexpr bool valid() const noexcept {
        return lead_blocks != 0 && pipeline_depth > lead_blocks && max_inflight != 0 &&
               max_inflight <= available_inflight();
    }
};

struct SharedIoExecutionContract {
    std::uint32_t channels = 0;
    std::uint32_t block_size = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t algorithmic_lead_blocks = 0;
    std::uint32_t pipeline_depth = 0;
    std::uint32_t provider_slots = 0;
    std::uint32_t max_inflight = 1;
    SharedIoRequest requested_path = SharedIoRequest::Auto;
    SharedIoPath active_path = SharedIoPath::Cpu;
    MissPolicy miss_policy = MissPolicy::Silence;
    bool shared_host_pointer_capable = false;
    bool cpu_fallback_prepared = false;
    SharedIoFallbackReason fallback_reason = SharedIoFallbackReason::None;
};

struct SharedIoContractValidation {
    SharedIoContractError error = SharedIoContractError::None;

    constexpr bool accepted() const noexcept {
        return error == SharedIoContractError::None;
    }
};

constexpr SharedIoContractValidation
validate_shared_io_contract(const SharedIoExecutionContract& contract) noexcept {
    const auto valid_path = [](SharedIoPath value) constexpr {
        return value == SharedIoPath::Cpu || value == SharedIoPath::StagedAsync ||
               value == SharedIoPath::SharedHostPointer;
    };
    const auto valid_request = [](SharedIoRequest value) constexpr {
        return value == SharedIoRequest::Auto ||
               value == SharedIoRequest::RequireSharedHostPointer ||
               value == SharedIoRequest::RequireStaged || value == SharedIoRequest::RequireCpu;
    };
    const auto valid_miss_policy = [](MissPolicy value) constexpr {
        return value == MissPolicy::Silence || value == MissPolicy::PassthroughDry ||
               value == MissPolicy::CpuFallback;
    };
    const auto valid_fallback_reason = [](SharedIoFallbackReason value) constexpr {
        return value == SharedIoFallbackReason::None ||
               value == SharedIoFallbackReason::UnsupportedProvider ||
               value == SharedIoFallbackReason::UnsupportedFeature ||
               value == SharedIoFallbackReason::ProviderMismatch ||
               value == SharedIoFallbackReason::AllocationFailed ||
               value == SharedIoFallbackReason::DeviceLost ||
               value == SharedIoFallbackReason::Timeout ||
               value == SharedIoFallbackReason::SubmissionRejected ||
               value == SharedIoFallbackReason::DeadlineExceeded ||
               value == SharedIoFallbackReason::InputSaturated ||
               value == SharedIoFallbackReason::SequenceGap ||
               value == SharedIoFallbackReason::Teardown ||
               value == SharedIoFallbackReason::CompletionFailed ||
               value == SharedIoFallbackReason::InvalidCallback ||
               value == SharedIoFallbackReason::PredictionRefused ||
               value == SharedIoFallbackReason::CalibrationMissing ||
               value == SharedIoFallbackReason::CalibrationStale;
    };
    if (!valid_path(contract.active_path) || !valid_request(contract.requested_path) ||
        !valid_miss_policy(contract.miss_policy) ||
        !valid_fallback_reason(contract.fallback_reason))
        return {SharedIoContractError::InvalidEnumValue};

    if (contract.channels == 0 || contract.block_size == 0 || contract.sample_rate == 0)
        return {SharedIoContractError::InvalidShape};

    const bool gpu_path = contract.active_path != SharedIoPath::Cpu;
    if (gpu_path && contract.algorithmic_lead_blocks == 0)
        return {SharedIoContractError::MissingAlgorithmicLead};
    if (gpu_path && contract.pipeline_depth == 0)
        return {SharedIoContractError::MissingPipelineDepth};
    if (gpu_path && contract.pipeline_depth <= contract.algorithmic_lead_blocks)
        return {SharedIoContractError::InsufficientPipelineDepth};
    if (gpu_path &&
        (contract.max_inflight == 0 ||
         contract.max_inflight > contract.pipeline_depth - contract.algorithmic_lead_blocks))
        return {SharedIoContractError::InvalidMaxInflight};
    if (gpu_path && contract.provider_slots == 0)
        return {SharedIoContractError::MissingProviderSlots};
    if (contract.active_path == SharedIoPath::SharedHostPointer &&
        !contract.shared_host_pointer_capable)
        return {SharedIoContractError::ProviderUnavailable};
    if (contract.miss_policy == MissPolicy::CpuFallback && !contract.cpu_fallback_prepared)
        return {SharedIoContractError::CpuFallbackNotPrepared};

    switch (contract.requested_path) {
    case SharedIoRequest::Auto:
        break;
    case SharedIoRequest::RequireSharedHostPointer:
        if (contract.active_path != SharedIoPath::SharedHostPointer)
            return {SharedIoContractError::RequestedPathUnavailable};
        break;
    case SharedIoRequest::RequireStaged:
        if (contract.active_path != SharedIoPath::StagedAsync)
            return {SharedIoContractError::RequestedPathUnavailable};
        break;
    case SharedIoRequest::RequireCpu:
        if (contract.active_path != SharedIoPath::Cpu)
            return {SharedIoContractError::RequestedPathUnavailable};
        break;
    }
    return {};
}

struct SharedIoTelemetrySnapshot {
    std::uint32_t configured_lead_blocks = 0;
    std::uint32_t configured_pipeline_depth = 0;
    std::uint32_t configured_max_inflight = 0;
    std::uint32_t configured_available_inflight = 0;
    std::uint64_t callback_blocks = 0;
    // Number of provider-bound work items admitted by the worker. This is
    // distinct from submitted_blocks because an admitted item may fail before
    // the provider accepts its submission.
    std::uint64_t admitted_blocks = 0;
    std::uint64_t submitted_blocks = 0;
    std::uint64_t retired_success = 0;
    std::uint64_t retired_failure = 0;
    std::uint64_t delivered_blocks = 0;
    std::uint64_t deadline_misses = 0;
    std::uint64_t late_completions = 0;
    std::uint64_t fallback_blocks = 0;
    std::uint64_t resync_drops = 0;
    std::uint64_t input_drops = 0;
    std::uint64_t payload_bytes_copied = 0;
    std::uint64_t in_flight_high_water = 0;
    std::uint64_t callback_duration_ns = 0;
    std::uint64_t worker_pack_copy_duration_ns = 0;
    std::uint64_t encode_submit_duration_ns = 0;
    std::uint64_t pre_submit_delay_ns = 0;
    std::uint64_t submit_to_completion_ns = 0;
    std::uint64_t scheduled_to_completion_ns = 0;
    // False means the completion-timing triple is not authoritative: either
    // the aggregate could not be represented without wrapping, or no coherent
    // copy could be taken because a writer held the update open.
    bool completion_timing_valid = true;
    std::uint64_t gpu_elapsed_ns = 0;
    std::uint64_t gpu_busy_counter = 0;
    bool gpu_elapsed_available = false;
};

enum class SharedIoTelemetryError : std::uint8_t {
    None,
    InvalidDepthConfiguration,
    OccupancyExceedsPipelineDepth,
    OccupancyExceedsMaxInflight,
    SubmittedExceedsAdmissions,
    SuccessfulRetirementsExceedSubmissions,
    RetirementsExceedAdmissions,
    DeliveredExceedsCallbacks,
    DeadlineMissesExceedCallbacks,
    LateCompletionsExceedRetirements,
    FallbacksExceedDelivered,
    ScheduledCompletionBeforeComponents,
    // Appended to preserve the numeric values of existing diagnostic errors.
    RetirementsExceedSubmissions,
};

struct SharedIoTelemetryValidation {
    SharedIoTelemetryError error = SharedIoTelemetryError::None;

    constexpr bool accepted() const noexcept {
        return error == SharedIoTelemetryError::None;
    }
};

// Validate a quiescent diagnostic snapshot without making claims about a
// concurrently updated counter set. A zeroed depth policy is valid because
// producers may expose telemetry before attaching a scheduling policy.
constexpr SharedIoTelemetryValidation
validate_shared_io_telemetry(const SharedIoTelemetrySnapshot& snapshot) noexcept {
    const bool depth_unconfigured =
        snapshot.configured_lead_blocks == 0 && snapshot.configured_pipeline_depth == 0 &&
        snapshot.configured_max_inflight == 0 && snapshot.configured_available_inflight == 0;
    if (!depth_unconfigured) {
        if (snapshot.configured_lead_blocks == 0 || snapshot.configured_pipeline_depth == 0 ||
            snapshot.configured_max_inflight == 0 ||
            snapshot.configured_pipeline_depth <= snapshot.configured_lead_blocks ||
            snapshot.configured_available_inflight !=
                snapshot.configured_pipeline_depth - snapshot.configured_lead_blocks ||
            snapshot.configured_max_inflight > snapshot.configured_available_inflight)
            return {SharedIoTelemetryError::InvalidDepthConfiguration};
    }
    if (snapshot.configured_pipeline_depth != 0 &&
        snapshot.in_flight_high_water > snapshot.configured_pipeline_depth)
        return {SharedIoTelemetryError::OccupancyExceedsPipelineDepth};
    if (snapshot.configured_max_inflight != 0 &&
        snapshot.in_flight_high_water > snapshot.configured_max_inflight)
        return {SharedIoTelemetryError::OccupancyExceedsMaxInflight};
    // Older controller-only producers use submitted_blocks for their
    // admission boundary and do not expose a separate admission count. Keep
    // those zero-admission snapshots compatible; traced providers opt into
    // the stricter relation by recording admissions explicitly.
    if (snapshot.admitted_blocks != 0 && snapshot.submitted_blocks > snapshot.admitted_blocks)
        return {SharedIoTelemetryError::SubmittedExceedsAdmissions};
    if (snapshot.retired_success > snapshot.submitted_blocks)
        return {SharedIoTelemetryError::SuccessfulRetirementsExceedSubmissions};
    if (snapshot.admitted_blocks == 0 &&
        (snapshot.retired_success > UINT64_MAX - snapshot.retired_failure ||
         snapshot.retired_success + snapshot.retired_failure > snapshot.submitted_blocks))
        return {SharedIoTelemetryError::RetirementsExceedSubmissions};
    if (snapshot.admitted_blocks != 0 &&
        (snapshot.retired_success > UINT64_MAX - snapshot.retired_failure ||
         snapshot.retired_success + snapshot.retired_failure > snapshot.admitted_blocks))
        return {SharedIoTelemetryError::RetirementsExceedAdmissions};
    if (snapshot.delivered_blocks > snapshot.callback_blocks)
        return {SharedIoTelemetryError::DeliveredExceedsCallbacks};
    if (snapshot.deadline_misses > snapshot.callback_blocks)
        return {SharedIoTelemetryError::DeadlineMissesExceedCallbacks};
    if (snapshot.retired_success > UINT64_MAX - snapshot.retired_failure ||
        snapshot.late_completions > snapshot.retired_success + snapshot.retired_failure)
        return {SharedIoTelemetryError::LateCompletionsExceedRetirements};
    if (snapshot.fallback_blocks > snapshot.delivered_blocks)
        return {SharedIoTelemetryError::FallbacksExceedDelivered};
    // `scheduled_to_completion_ns` is the sum of the two component samples.
    // A wrapped sum would otherwise look like a valid, tiny latency receipt
    // and could hide a provider timing failure from health consumers.
    if (snapshot.scheduled_to_completion_ns < snapshot.pre_submit_delay_ns ||
        snapshot.scheduled_to_completion_ns < snapshot.submit_to_completion_ns)
        return {SharedIoTelemetryError::ScheduledCompletionBeforeComponents};
    return {};
}

// Atomic, allocation-free counters for the private bridge. Timing values are
// the latest observed sample and a snapshot may span adjacent worker updates,
// except that the completion-timing triple is copied under a writer-serialized
// sequence guard; the bridge's bounded raw receipt remains the authority for
// correlated distributions. GPU elapsed time is explicitly unavailable until an
// authentic provider timestamp is supplied.
class SharedIoTelemetry {
  public:
    void configure_depth_lead(SharedIoDepthLeadPolicy policy) noexcept {
        configured_lead_blocks_.store(policy.lead_blocks, std::memory_order_relaxed);
        configured_pipeline_depth_.store(policy.pipeline_depth, std::memory_order_relaxed);
        configured_max_inflight_.store(policy.max_inflight, std::memory_order_relaxed);
        configured_available_inflight_.store(policy.available_inflight(),
                                             std::memory_order_relaxed);
    }

    // Non-realtime lifecycle operation: it may wait for an in-progress
    // completion-timing record so that the reset is never dropped.
    void reset() noexcept {
        configured_lead_blocks_.store(0, std::memory_order_relaxed);
        configured_pipeline_depth_.store(0, std::memory_order_relaxed);
        configured_max_inflight_.store(0, std::memory_order_relaxed);
        configured_available_inflight_.store(0, std::memory_order_relaxed);
        callback_blocks_.store(0, std::memory_order_relaxed);
        admitted_blocks_.store(0, std::memory_order_relaxed);
        submitted_blocks_.store(0, std::memory_order_relaxed);
        retired_success_.store(0, std::memory_order_relaxed);
        retired_failure_.store(0, std::memory_order_relaxed);
        delivered_blocks_.store(0, std::memory_order_relaxed);
        deadline_misses_.store(0, std::memory_order_relaxed);
        late_completions_.store(0, std::memory_order_relaxed);
        fallback_blocks_.store(0, std::memory_order_relaxed);
        resync_drops_.store(0, std::memory_order_relaxed);
        input_drops_.store(0, std::memory_order_relaxed);
        payload_bytes_copied_.store(0, std::memory_order_relaxed);
        in_flight_high_water_.store(0, std::memory_order_relaxed);
        callback_duration_ns_.store(0, std::memory_order_relaxed);
        worker_pack_copy_duration_ns_.store(0, std::memory_order_relaxed);
        encode_submit_duration_ns_.store(0, std::memory_order_relaxed);
        begin_completion_timing_update();
        pre_submit_delay_ns_.store(0, std::memory_order_relaxed);
        submit_to_completion_ns_.store(0, std::memory_order_relaxed);
        scheduled_to_completion_ns_.store(0, std::memory_order_relaxed);
        completion_timing_valid_.store(true, std::memory_order_relaxed);
        completion_timing_sequence_.fetch_add(1, std::memory_order_release);
        gpu_elapsed_ns_.store(0, std::memory_order_relaxed);
        gpu_busy_counter_.store(0, std::memory_order_relaxed);
        gpu_elapsed_available_.store(false, std::memory_order_relaxed);
    }

    void record_callback_block(bool deadline_miss) noexcept {
        callback_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (deadline_miss)
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_admission() noexcept {
        admitted_blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_deadline_miss() noexcept {
        deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_submit() noexcept {
        submitted_blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_retired(bool success) noexcept {
        (success ? retired_success_ : retired_failure_).fetch_add(1, std::memory_order_relaxed);
    }
    void record_delivery(bool fallback, bool late) noexcept {
        delivered_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (fallback)
            fallback_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (late)
            late_completions_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_late_completion() noexcept {
        late_completions_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_resync_drop() noexcept {
        resync_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_input_drop() noexcept {
        input_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_payload_copy(std::uint64_t bytes) noexcept {
        payload_bytes_copied_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void observe_in_flight(std::uint64_t count) noexcept {
        auto current = in_flight_high_water_.load(std::memory_order_relaxed);
        while (current < count &&
               !in_flight_high_water_.compare_exchange_weak(
                   current, count, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }
    void record_callback_duration_ns(std::uint64_t value) noexcept {
        callback_duration_ns_.store(value, std::memory_order_relaxed);
    }
    void record_worker_pack_copy_duration_ns(std::uint64_t value) noexcept {
        worker_pack_copy_duration_ns_.store(value, std::memory_order_relaxed);
    }
    void record_encode_submit_duration_ns(std::uint64_t value) noexcept {
        encode_submit_duration_ns_.store(value, std::memory_order_relaxed);
    }
    // Returns false when the sample was dropped because a concurrent writer held
    // the guard; that writer's update is the latest one instead, and dropping
    // keeps this path wait-bounded.
    bool record_completion_timing(std::uint64_t pre_submit_delay_ns,
                                  std::uint64_t submit_to_completion_ns) noexcept {
        if (!try_begin_completion_timing_update())
            return false;
        pre_submit_delay_ns_.store(pre_submit_delay_ns, std::memory_order_relaxed);
        submit_to_completion_ns_.store(submit_to_completion_ns, std::memory_order_relaxed);
        if (submit_to_completion_ns >
            std::numeric_limits<std::uint64_t>::max() - pre_submit_delay_ns) {
            scheduled_to_completion_ns_.store(0, std::memory_order_relaxed);
            completion_timing_valid_.store(false, std::memory_order_relaxed);
            completion_timing_sequence_.fetch_add(1, std::memory_order_release);
            return true;
        }
        scheduled_to_completion_ns_.store(pre_submit_delay_ns + submit_to_completion_ns,
                                          std::memory_order_relaxed);
        completion_timing_valid_.store(true, std::memory_order_relaxed);
        completion_timing_sequence_.fetch_add(1, std::memory_order_release);
        return true;
    }
    void record_gpu_elapsed_ns(std::uint64_t value) noexcept {
        gpu_elapsed_ns_.store(value, std::memory_order_relaxed);
        gpu_elapsed_available_.store(true, std::memory_order_release);
    }
    void record_gpu_busy_counter(std::uint64_t value) noexcept {
        gpu_busy_counter_.store(value, std::memory_order_relaxed);
    }

    SharedIoTelemetrySnapshot snapshot() const noexcept {
        SharedIoTelemetrySnapshot out;
        out.configured_lead_blocks = configured_lead_blocks_.load(std::memory_order_relaxed);
        out.configured_pipeline_depth = configured_pipeline_depth_.load(std::memory_order_relaxed);
        out.configured_max_inflight = configured_max_inflight_.load(std::memory_order_relaxed);
        out.configured_available_inflight =
            configured_available_inflight_.load(std::memory_order_relaxed);
        out.callback_blocks = callback_blocks_.load(std::memory_order_relaxed);
        out.admitted_blocks = admitted_blocks_.load(std::memory_order_relaxed);
        out.submitted_blocks = submitted_blocks_.load(std::memory_order_relaxed);
        out.retired_success = retired_success_.load(std::memory_order_relaxed);
        out.retired_failure = retired_failure_.load(std::memory_order_relaxed);
        out.delivered_blocks = delivered_blocks_.load(std::memory_order_relaxed);
        out.deadline_misses = deadline_misses_.load(std::memory_order_relaxed);
        out.late_completions = late_completions_.load(std::memory_order_relaxed);
        out.fallback_blocks = fallback_blocks_.load(std::memory_order_relaxed);
        out.resync_drops = resync_drops_.load(std::memory_order_relaxed);
        out.input_drops = input_drops_.load(std::memory_order_relaxed);
        out.payload_bytes_copied = payload_bytes_copied_.load(std::memory_order_relaxed);
        out.in_flight_high_water = in_flight_high_water_.load(std::memory_order_relaxed);
        out.callback_duration_ns = callback_duration_ns_.load(std::memory_order_relaxed);
        out.worker_pack_copy_duration_ns =
            worker_pack_copy_duration_ns_.load(std::memory_order_relaxed);
        out.encode_submit_duration_ns = encode_submit_duration_ns_.load(std::memory_order_relaxed);
        out.completion_timing_valid = false;
        for (int attempt = 0; attempt < kCompletionTimingAttempts; ++attempt) {
            const auto sequence = completion_timing_sequence_.load(std::memory_order_acquire);
            if ((sequence & 1U) != 0U) {
                continue;
            }
            const auto pre_submit = pre_submit_delay_ns_.load(std::memory_order_relaxed);
            const auto submit = submit_to_completion_ns_.load(std::memory_order_relaxed);
            const auto scheduled = scheduled_to_completion_ns_.load(std::memory_order_relaxed);
            const auto valid = completion_timing_valid_.load(std::memory_order_relaxed);
            // Orders the relaxed data loads before the re-check; an acquire
            // load alone would let them be satisfied after it.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (completion_timing_sequence_.load(std::memory_order_relaxed) == sequence) {
                out.pre_submit_delay_ns = pre_submit;
                out.submit_to_completion_ns = submit;
                out.scheduled_to_completion_ns = scheduled;
                out.completion_timing_valid = valid;
                break;
            }
        }
        out.gpu_elapsed_available = gpu_elapsed_available_.load(std::memory_order_acquire);
        out.gpu_elapsed_ns = gpu_elapsed_ns_.load(std::memory_order_relaxed);
        out.gpu_busy_counter = gpu_busy_counter_.load(std::memory_order_relaxed);
        return out;
    }

  private:
    // Bounds both sides of the completion-timing guard. A snapshot that cannot
    // observe a stable sequence within this many attempts reports the triple as
    // invalid, and a recorder that cannot acquire it drops its sample.
    static constexpr int kCompletionTimingAttempts = 64;

    bool try_begin_completion_timing_update() noexcept {
        for (int attempt = 0; attempt < kCompletionTimingAttempts; ++attempt) {
            auto sequence = completion_timing_sequence_.load(std::memory_order_relaxed);
            if ((sequence & 1U) == 0U &&
                completion_timing_sequence_.compare_exchange_weak(
                    sequence, sequence + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                // Keeps the following data stores from becoming visible before
                // the odd sequence value that marks the update as in progress.
                std::atomic_thread_fence(std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    void begin_completion_timing_update() noexcept {
        while (!try_begin_completion_timing_update()) {
        }
    }

    std::atomic<std::uint64_t> callback_blocks_{0}, admitted_blocks_{0}, submitted_blocks_{0};
    std::atomic<std::uint64_t> retired_success_{0}, retired_failure_{0};
    std::atomic<std::uint64_t> delivered_blocks_{0}, deadline_misses_{0};
    std::atomic<std::uint64_t> late_completions_{0}, fallback_blocks_{0};
    std::atomic<std::uint64_t> resync_drops_{0}, input_drops_{0};
    std::atomic<std::uint32_t> configured_lead_blocks_{0}, configured_pipeline_depth_{0},
        configured_max_inflight_{0}, configured_available_inflight_{0};
    std::atomic<std::uint64_t> payload_bytes_copied_{0}, in_flight_high_water_{0};
    std::atomic<std::uint64_t> callback_duration_ns_{0}, worker_pack_copy_duration_ns_{0};
    std::atomic<std::uint64_t> encode_submit_duration_ns_{0}, pre_submit_delay_ns_{0};
    std::atomic<std::uint64_t> submit_to_completion_ns_{0}, scheduled_to_completion_ns_{0};
    std::atomic<bool> completion_timing_valid_{true};
    std::atomic<std::uint64_t> completion_timing_sequence_{0};
    std::atomic<std::uint64_t> gpu_elapsed_ns_{0}, gpu_busy_counter_{0};
    std::atomic<bool> gpu_elapsed_available_{false};
};

} // namespace pulp::gpu_audio::detail
