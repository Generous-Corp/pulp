#pragma once

#include <atomic>
#include <cstdint>

#include <pulp/gpu_audio/gpu_audio_node.hpp>

namespace pulp::gpu_audio::detail {

// This is deliberately private until the installed SDK contract is settled.
// `algorithmic_lead_blocks` is the number of complete blocks by which a result
// may trail the callback timeline; `pipeline_depth` is only the number of
// physical shared slots. They are distinct knobs, but a GPU path needs at
// least one physical slot for every block of declared lead.
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
};

struct SharedIoExecutionContract {
    std::uint32_t channels = 0;
    std::uint32_t block_size = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t algorithmic_lead_blocks = 0;
    std::uint32_t pipeline_depth = 0;
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
    if (contract.channels == 0 || contract.block_size == 0 || contract.sample_rate == 0)
        return {SharedIoContractError::InvalidShape};

    const bool gpu_path = contract.active_path != SharedIoPath::Cpu;
    if (gpu_path && contract.algorithmic_lead_blocks == 0)
        return {SharedIoContractError::MissingAlgorithmicLead};
    if (gpu_path && contract.pipeline_depth == 0)
        return {SharedIoContractError::MissingPipelineDepth};
    if (gpu_path && contract.pipeline_depth < contract.algorithmic_lead_blocks)
        return {SharedIoContractError::InsufficientPipelineDepth};
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
    std::uint64_t callback_blocks = 0;
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
    std::uint64_t gpu_elapsed_ns = 0;
    std::uint64_t gpu_busy_counter = 0;
    bool gpu_elapsed_available = false;
};

// Atomic, allocation-free counters for the private bridge. Timing values are
// the latest observed sample and a snapshot may span adjacent worker updates;
// the bridge's bounded raw receipt remains the authority for correlated
// distributions. GPU elapsed time is explicitly unavailable until an
// authentic provider timestamp is supplied.
class SharedIoTelemetry {
  public:
    void reset() noexcept {
        callback_blocks_.store(0, std::memory_order_relaxed);
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
        pre_submit_delay_ns_.store(0, std::memory_order_relaxed);
        submit_to_completion_ns_.store(0, std::memory_order_relaxed);
        scheduled_to_completion_ns_.store(0, std::memory_order_relaxed);
        gpu_elapsed_ns_.store(0, std::memory_order_relaxed);
        gpu_busy_counter_.store(0, std::memory_order_relaxed);
        gpu_elapsed_available_.store(false, std::memory_order_relaxed);
    }

    void record_callback_block(bool deadline_miss) noexcept {
        callback_blocks_.fetch_add(1, std::memory_order_relaxed);
        if (deadline_miss)
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
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
    void record_completion_timing(std::uint64_t pre_submit_delay_ns,
                                  std::uint64_t submit_to_completion_ns) noexcept {
        pre_submit_delay_ns_.store(pre_submit_delay_ns, std::memory_order_relaxed);
        submit_to_completion_ns_.store(submit_to_completion_ns, std::memory_order_relaxed);
        scheduled_to_completion_ns_.store(pre_submit_delay_ns + submit_to_completion_ns,
                                          std::memory_order_relaxed);
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
        out.callback_blocks = callback_blocks_.load(std::memory_order_relaxed);
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
        out.pre_submit_delay_ns = pre_submit_delay_ns_.load(std::memory_order_relaxed);
        out.submit_to_completion_ns = submit_to_completion_ns_.load(std::memory_order_relaxed);
        out.scheduled_to_completion_ns =
            scheduled_to_completion_ns_.load(std::memory_order_relaxed);
        out.gpu_elapsed_available = gpu_elapsed_available_.load(std::memory_order_acquire);
        out.gpu_elapsed_ns = gpu_elapsed_ns_.load(std::memory_order_relaxed);
        out.gpu_busy_counter = gpu_busy_counter_.load(std::memory_order_relaxed);
        return out;
    }

  private:
    std::atomic<std::uint64_t> callback_blocks_{0}, submitted_blocks_{0};
    std::atomic<std::uint64_t> retired_success_{0}, retired_failure_{0};
    std::atomic<std::uint64_t> delivered_blocks_{0}, deadline_misses_{0};
    std::atomic<std::uint64_t> late_completions_{0}, fallback_blocks_{0};
    std::atomic<std::uint64_t> resync_drops_{0}, input_drops_{0};
    std::atomic<std::uint64_t> payload_bytes_copied_{0}, in_flight_high_water_{0};
    std::atomic<std::uint64_t> callback_duration_ns_{0}, worker_pack_copy_duration_ns_{0};
    std::atomic<std::uint64_t> encode_submit_duration_ns_{0}, pre_submit_delay_ns_{0};
    std::atomic<std::uint64_t> submit_to_completion_ns_{0}, scheduled_to_completion_ns_{0};
    std::atomic<std::uint64_t> gpu_elapsed_ns_{0}, gpu_busy_counter_{0};
    std::atomic<bool> gpu_elapsed_available_{false};
};

} // namespace pulp::gpu_audio::detail
