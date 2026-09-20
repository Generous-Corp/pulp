#pragma once

#include "shared_io_execution_contract.hpp"

#include <pulp/runtime/spsc_queue.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pulp::gpu_audio::detail {

// CPU timestamps in a record share the worker's monotonic clock. Perfetto's
// instant timestamp is drain time, not a backdated GPU or callback timestamp.
enum class SharedIoTraceStage : std::uint8_t {
    Scheduled,
    WorkerEntry,
    EncodeBegin,
    EncodeEnd,
    SubmitBegin,
    SubmitEnd,
    CompletionObserved,
    Count,
};
inline constexpr std::size_t kSharedIoTraceStageCount =
    static_cast<std::size_t>(SharedIoTraceStage::Count);

enum class SharedIoTraceOutcome : std::uint8_t {
    Success,
    SubmissionRejected,
    CompletionFailed,
    DeadlineExceeded,
    StaleRejected,
    LateRejected,
    DeviceLost,
    Cancelled,
};

// These lifecycles are intentionally orthogonal. A GPU result may be late or
// stale while the corresponding CPU fallback is delivered; collapsing those
// facts into one status loses the cause needed to debug a glitch.
enum class SharedIoGpuTerminalDisposition : std::uint8_t {
    None,
    CompletedAccepted,
    StaleRejected,
    LateRejected,
    ProviderFailed,
    DeviceLost,
    CancelledTeardown,
};
enum class SharedIoDeliveryDisposition : std::uint8_t {
    None,
    GpuDelivered,
    CpuFallbackDelivered,
    SilenceDelivered,
    PassthroughDelivered,
    Priming,
    InvalidRejected,
};

enum class SharedIoTraceKind : std::uint8_t { Terminal, Eligible, Delivery, Recovery };

struct SharedIoTraceRecord {
    SharedIoTraceKind kind = SharedIoTraceKind::Terminal;
    std::uint64_t next_generation = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, kSharedIoTraceStageCount> cpu_ns{};
    std::uint32_t valid_stages = 0;
    SharedIoTraceOutcome outcome = SharedIoTraceOutcome::Success;
    // The compatibility summary reason follows the delivered-audio cause when
    // present, otherwise the GPU terminal cause. Consumers that need causality
    // must use the two first-class fields below.
    SharedIoFallbackReason reason = SharedIoFallbackReason::None;
    SharedIoFallbackReason gpu_reason = SharedIoFallbackReason::None;
    SharedIoFallbackReason delivery_reason = SharedIoFallbackReason::None;
    SharedIoGpuTerminalDisposition gpu_terminal = SharedIoGpuTerminalDisposition::None;
    SharedIoDeliveryDisposition delivery = SharedIoDeliveryDisposition::None;
    bool gpu_work_admitted = false;
    bool output_eligible = false;
    std::uint64_t gpu_elapsed_ns = 0;
    bool gpu_elapsed_available = false;

    void set(SharedIoTraceStage stage, std::uint64_t time_ns) noexcept {
        const auto i = static_cast<std::size_t>(stage);
        if (i >= cpu_ns.size())
            return;
        cpu_ns[i] = time_ns;
        valid_stages |= 1u << i;
    }
    bool has(SharedIoTraceStage stage) const noexcept {
        const auto i = static_cast<std::size_t>(stage);
        return i < cpu_ns.size() && (valid_stages & (1u << i)) != 0;
    }
    bool valid() const noexcept;
};
static_assert(std::is_trivially_copyable_v<SharedIoTraceRecord>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// Zero is never a measured replacement for an absent timestamp. Consumers
// must test available, or use the emitter's -1 sentinel / SQL NULL projection.
struct SharedIoTraceDuration {
    std::uint64_t ns = 0;
    bool available = false;
};
SharedIoTraceDuration shared_io_trace_duration(const SharedIoTraceRecord& record,
                                               SharedIoTraceStage begin,
                                               SharedIoTraceStage end) noexcept;
const char* shared_io_gpu_terminal_name(SharedIoGpuTerminalDisposition) noexcept;
const char* shared_io_delivery_name(SharedIoDeliveryDisposition) noexcept;
const char* shared_io_trace_outcome_name(SharedIoTraceOutcome) noexcept;
const char* shared_io_fallback_reason_name(SharedIoFallbackReason) noexcept;

struct SharedIoTraceConfig {
    std::uint64_t engine_id = 0;
    std::uint64_t generation = 0;
    SharedIoExecutionContract contract;
    std::uint32_t success_stride = 64;
    // Diagnostic full-lifecycle mode. Every admission is written to a separate
    // dispatcher->diagnostic SPSC queue so a lossless stride-1 capture can
    // prove missing/orphaned terminal identities mechanically.
    bool capture_admissions = false;
    bool enabled = false;
};

struct SharedIoTraceStats {
    std::uint64_t admissions_attempted = 0;
    std::uint64_t admissions_enqueued = 0;
    std::uint64_t admissions_dropped = 0;
    std::uint64_t admissions_drained = 0;
    std::uint64_t attempted = 0;
    std::uint64_t sampled_out = 0;
    std::uint64_t invalid = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t dropped = 0;
    std::uint64_t drained = 0;
};

struct SharedIoTraceAdmission {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
};
static_assert(std::is_trivially_copyable_v<SharedIoTraceAdmission>);

// Construct and destroy after callback and worker quiescence. The session worker
// and callback each own a distinct SPSC queue; one diagnostic thread drains both. Callback code
// may perform one bounded SPSC publish of an already-complete record; it never
// calls Perfetto, allocates, locks, waits, or checks global trace state.
// Config and generation are immutable; prepare a new recorder after quiescence
// for a new engine generation. Records cannot leak across a reset boundary.
class SharedIoTraceRecorder {
  public:
    static constexpr std::size_t capacity = 256;
    explicit SharedIoTraceRecorder(const SharedIoTraceConfig& config) : config_(config) {}
    SharedIoTraceRecorder(const SharedIoTraceRecorder&) = delete;
    SharedIoTraceRecorder& operator=(const SharedIoTraceRecorder&) = delete;

    bool enabled() const noexcept {
        return config_.enabled && config_.engine_id != 0 && config_.generation != 0 &&
               config_.success_stride != 0 &&
               validate_shared_io_contract(config_.contract).accepted();
    }
    const SharedIoTraceConfig& config() const noexcept {
        return config_;
    }
    bool publish_completed(const SharedIoTraceRecord& record) noexcept;
    bool publish_callback(const SharedIoTraceRecord& record) noexcept;
    bool publish_admission(std::uint64_t generation, std::uint64_t sequence) noexcept;
    // Compatibility name for worker-only probes that already own a complete
    // record. Session terminal transitions use publish_completed().
    bool publish_worker(const SharedIoTraceRecord& record) noexcept {
        return publish_completed(record);
    }
    SharedIoTraceStats stats() const noexcept;

    // Consumer only. The budget bounds every call even during a producer storm.
    // The sink is deliberately supplied by the non-RT caller, not the producer.
    template <class Sink> std::uint32_t drain_worker_records(std::uint32_t budget, Sink&& sink) {
        const auto limit = budget < capacity ? budget : static_cast<std::uint32_t>(capacity);
        SharedIoTraceRecord record;
        std::uint32_t count = 0;
        while (count < limit) {
            auto& first = callback_first_ ? callback_queue_ : queue_;
            auto& second = callback_first_ ? queue_ : callback_queue_;
            callback_first_ = !callback_first_;
            if (!first.try_pop(record) && !second.try_pop(record))
                break;
            sink(record);
            ++count;
            drained_.fetch_add(1, std::memory_order_relaxed);
        }
        return count;
    }

    template <class Sink> std::uint32_t drain_admissions(std::uint32_t budget, Sink&& sink) {
        const auto limit = budget < capacity ? budget : static_cast<std::uint32_t>(capacity);
        SharedIoTraceAdmission admission;
        std::uint32_t count = 0;
        while (count < limit && admissions_.try_pop(admission)) {
            sink(admission);
            ++count;
            admissions_drained_.fetch_add(1, std::memory_order_relaxed);
        }
        return count;
    }

    // Consumer only. Alternating the reserved first half prevents either queue
    // from being permanently starved, including when the caller supplies a
    // one-record budget during a sustained producer storm.
    bool terminals_first_for_next_drain() noexcept {
        const bool value = terminals_first_;
        terminals_first_ = !terminals_first_;
        return value;
    }

  private:
    bool publish(const SharedIoTraceRecord&, bool callback) noexcept;
    const SharedIoTraceConfig config_;
    runtime::SpscQueue<SharedIoTraceRecord, capacity> queue_;
    runtime::SpscQueue<SharedIoTraceAdmission, capacity> admissions_;
    runtime::SpscQueue<SharedIoTraceRecord, capacity> callback_queue_;
    bool callback_first_ = false;
    std::atomic<std::uint64_t> admissions_attempted_{0}, admissions_enqueued_{0};
    std::atomic<std::uint64_t> admissions_drained_{0};
    std::atomic<std::uint64_t> attempted_{0}, sampled_out_{0}, invalid_{0};
    std::atomic<std::uint64_t> enqueued_{0}, drained_{0};
    bool terminals_first_ = true;
};

struct SharedIoTraceDrainResult {
    bool tracing_compiled = false;
    bool recording_enabled = false;
    std::uint32_t records_drained = 0;
    // Emission attempts do not prove an active session or enabled category.
    // The flushed trace and its session marker are the positive control.
    std::uint32_t emission_attempts = 0;
};

// Non-RT diagnostic thread ONLY, including when the producer becomes a future
// realtime auxiliary worker. This is the sole Perfetto edge for these records.
// Counters are approximate cumulative snapshots; no per-block timing is
// inferred from the independent "latest sample" atomics.
SharedIoTraceDrainResult drain_shared_io_trace(SharedIoTraceRecorder& recorder,
                                               const SharedIoTelemetrySnapshot& telemetry,
                                               std::uint32_t budget = 256) noexcept;

} // namespace pulp::gpu_audio::detail
