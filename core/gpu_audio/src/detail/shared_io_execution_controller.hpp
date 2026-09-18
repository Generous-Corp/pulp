#pragma once

#include "shared_io_execution_contract.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::gpu_audio::detail {

// This controller is the Dawn-free reducer between a provider completion and
// the audio callback. Storage is allocated only by prepare(); every operation
// used by the callback is bounded by the prepared pipeline depth and does not
// allocate, lock, or wait.
enum class SharedIoAdmission : std::uint8_t {
    Accepted,
    NotPrepared,
    SequenceGap,
    CapacityFull,
};

enum class SharedIoCompletion : std::uint8_t { Success, Failed };

enum class SharedIoDeliveryPath : std::uint8_t {
    Priming,
    Gpu,
    CpuFallback,
    Silence,
    PassthroughDry,
};

struct SharedIoDelivery {
    SharedIoDeliveryPath path = SharedIoDeliveryPath::Silence;
    std::uint64_t callback_sequence = 0;
    std::uint64_t expected_sequence = 0;
    SharedIoFallbackReason fallback_reason = SharedIoFallbackReason::None;
    bool resynced = false;

    constexpr bool has_gpu_output() const noexcept {
        return path == SharedIoDeliveryPath::Gpu;
    }
    constexpr bool uses_cpu_fallback() const noexcept {
        return path == SharedIoDeliveryPath::CpuFallback;
    }
    constexpr bool uses_fallback() const noexcept {
        return path == SharedIoDeliveryPath::CpuFallback || path == SharedIoDeliveryPath::Silence ||
               path == SharedIoDeliveryPath::PassthroughDry;
    }
};

class SharedIoExecutionController {
  public:
#if defined(PULP_GPU_AUDIO_CONTROLLER_TEST_HOOKS)
    using BeforePublishTestHook = void (*)(SharedIoExecutionController&, std::uint64_t,
                                           void*) noexcept;
    using BeforeReuseTestHook = void (*)(SharedIoExecutionController&, std::uint32_t,
                                         void*) noexcept;
#endif

    SharedIoExecutionController() = default;
    ~SharedIoExecutionController() = default;
    SharedIoExecutionController(const SharedIoExecutionController&) = delete;
    SharedIoExecutionController& operator=(const SharedIoExecutionController&) = delete;

    // Host/quiescent only. The contract is copied by value so callback code
    // never consults a mutable node descriptor. The physical pipeline depth
    // is the only completion-table capacity and must cover the declared lead.
    bool prepare(const SharedIoExecutionContract& contract,
                 SharedIoTelemetry* telemetry = nullptr) {
        if (prepared_ || !validate_shared_io_contract(contract).accepted() ||
            contract.active_path == SharedIoPath::Cpu || contract.pipeline_depth == 0)
            return false;
        try {
            entries_ = std::make_unique<Entry[]>(contract.pipeline_depth);
        } catch (...) {
            return false;
        }
        contract_ = contract;
        capacity_ = contract.pipeline_depth;
        telemetry_ = telemetry;
        next_admission_sequence_.store(0, std::memory_order_relaxed);
        next_callback_sequence_ = 0;
        callback_started_ = false;
        in_flight_.store(0, std::memory_order_relaxed);
        prepared_ = true;
        return true;
    }

    // Host/quiescent only. Callers must first stop submissions and callbacks.
    void release() noexcept {
        entries_.reset();
        capacity_ = 0;
        prepared_ = false;
        telemetry_ = nullptr;
        in_flight_.store(0, std::memory_order_relaxed);
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    std::uint32_t capacity() const noexcept {
        return capacity_;
    }
    std::uint32_t algorithmic_lead_blocks() const noexcept {
        return contract_.algorithmic_lead_blocks;
    }

#if defined(PULP_GPU_AUDIO_CONTROLLER_TEST_HOOKS)
    void set_before_publish_test_hook(BeforePublishTestHook hook, void* context) noexcept {
        before_publish_test_hook_ = hook;
        before_publish_test_context_ = context;
    }
    void set_before_reuse_test_hook(BeforeReuseTestHook hook, void* context) noexcept {
        before_reuse_test_hook_ = hook;
        before_reuse_test_context_ = context;
    }
    std::uint32_t in_flight_for_testing() const noexcept {
        return in_flight_.load(std::memory_order_relaxed);
    }
#endif

    // Non-RT dispatcher operation. Admission is strictly ordered so a missing
    // input cannot silently move the logical stream onto another sequence.
    SharedIoAdmission admit_submission(std::uint64_t sequence) noexcept {
        if (!prepared_)
            return SharedIoAdmission::NotPrepared;
        if (sequence != next_admission_sequence_.load(std::memory_order_acquire)) {
            if (telemetry_)
                telemetry_->record_resync_drop();
            return SharedIoAdmission::SequenceGap;
        }

        for (std::uint32_t index = 0; index < capacity_; ++index) {
            Entry& entry = entries_[index];
            std::uint8_t expected = static_cast<std::uint8_t>(EntryState::Empty);
            if (!entry.state.compare_exchange_strong(
                    expected, static_cast<std::uint8_t>(EntryState::Writing),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            entry.sequence = sequence;
            entry.completion = SharedIoCompletion::Failed;
            entry.failure_reason = SharedIoFallbackReason::SubmissionRejected;
            auto expected_sequence = sequence;
            if (!next_admission_sequence_.compare_exchange_strong(expected_sequence, sequence + 1,
                                                                  std::memory_order_acq_rel,
                                                                  std::memory_order_acquire)) {
                entry.state.store(static_cast<std::uint8_t>(EntryState::Empty),
                                  std::memory_order_release);
                if (telemetry_)
                    telemetry_->record_resync_drop();
                return SharedIoAdmission::SequenceGap;
            }
            // Occupancy must be reserved before the release-store publishes
            // the entry to provider and callback consumers.
            const auto count = in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(PULP_GPU_AUDIO_CONTROLLER_TEST_HOOKS)
            if (before_publish_test_hook_)
                before_publish_test_hook_(*this, sequence, before_publish_test_context_);
#endif
            entry.state.store(static_cast<std::uint8_t>(EntryState::Admitted),
                              std::memory_order_release);
            if (telemetry_) {
                telemetry_->record_submit();
                telemetry_->observe_in_flight(count);
            }
            return SharedIoAdmission::Accepted;
        }
        // Saturation is retryable backpressure. The owner records an input
        // drop only if it abandons this sequence.
        return SharedIoAdmission::CapacityFull;
    }

    // Provider completion operation. A completion for an unadmitted, stale, or
    // duplicate sequence is rejected without mutating another sequence.
    bool record_completion(
        std::uint64_t sequence, SharedIoCompletion completion,
        SharedIoFallbackReason failure_reason = SharedIoFallbackReason::None) noexcept {
        if (!prepared_)
            return false;
        for (std::uint32_t index = 0; index < capacity_; ++index) {
            Entry& entry = entries_[index];
            if (entry.state.load(std::memory_order_acquire) !=
                static_cast<std::uint8_t>(EntryState::Admitted))
                continue;
            if (entry.sequence != sequence)
                continue;
            std::uint8_t expected = static_cast<std::uint8_t>(EntryState::Admitted);
            if (!entry.state.compare_exchange_strong(
                    expected, static_cast<std::uint8_t>(EntryState::Completing),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            entry.completion = completion;
            entry.failure_reason = completion == SharedIoCompletion::Success
                                       ? SharedIoFallbackReason::None
                                       : (failure_reason == SharedIoFallbackReason::None
                                              ? SharedIoFallbackReason::SubmissionRejected
                                              : failure_reason);
            entry.state.store(static_cast<std::uint8_t>(EntryState::Ready),
                              std::memory_order_release);
            if (telemetry_)
                telemetry_->record_retired(completion == SharedIoCompletion::Success);
            return true;
        }
        if (telemetry_)
            telemetry_->record_resync_drop();
        return false;
    }

    // Audio callback operation. It only performs bounded atomic scans and
    // relaxed telemetry; no callback path can allocate, lock, or block.
    SharedIoDelivery deliver(std::uint64_t callback_sequence) noexcept {
        SharedIoDelivery result;
        result.callback_sequence = callback_sequence;

        if (!prepared_) {
            result.fallback_reason = SharedIoFallbackReason::Teardown;
            return result;
        }
        if (telemetry_)
            telemetry_->record_callback_block(false);
        if (callback_started_ && callback_sequence != next_callback_sequence_) {
            const bool forward_gap = callback_sequence > next_callback_sequence_;
            // A timeline discontinuity is itself a miss. Keep the declared
            // miss policy (CPU fallback, silence, or dry passthrough) rather
            // than relying on SharedIoDelivery's default Silence value.
            result.path = fallback_path();
            result.fallback_reason = SharedIoFallbackReason::SequenceGap;
            result.resynced = forward_gap;
            if (telemetry_)
                telemetry_->record_resync_drop();
            if (telemetry_)
                telemetry_->record_delivery(true, false);
            if (forward_gap) {
                const auto first_future_sequence =
                    callback_sequence >= contract_.algorithmic_lead_blocks
                        ? callback_sequence - contract_.algorithmic_lead_blocks + 1
                        : 0;
                resync_admission(first_future_sequence);
                discard_ready_entries_before(first_future_sequence);
                next_callback_sequence_ = callback_sequence + 1;
            }
            return result;
        }
        callback_started_ = true;
        next_callback_sequence_ = callback_sequence + 1;

        if (callback_sequence < contract_.algorithmic_lead_blocks) {
            result.path = SharedIoDeliveryPath::Priming;
            if (telemetry_)
                telemetry_->record_delivery(false, false);
            return result;
        }
        result.expected_sequence = callback_sequence - contract_.algorithmic_lead_blocks;

        bool late = false;
        for (std::uint32_t index = 0; index < capacity_; ++index) {
            Entry& entry = entries_[index];
            const auto state = entry.state.load(std::memory_order_acquire);
            if (state == static_cast<std::uint8_t>(EntryState::Ready) &&
                entry.sequence < result.expected_sequence) {
                std::uint8_t expected = static_cast<std::uint8_t>(EntryState::Ready);
                if (entry.state.compare_exchange_strong(
                        expected, static_cast<std::uint8_t>(EntryState::Claimed),
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    retire_claimed_entry(entry);
                    late = true;
                    if (telemetry_) {
                        telemetry_->record_resync_drop();
                        telemetry_->record_late_completion();
                    }
                }
                continue;
            }
            if (state != static_cast<std::uint8_t>(EntryState::Ready) ||
                entry.sequence != result.expected_sequence)
                continue;
            std::uint8_t expected = static_cast<std::uint8_t>(EntryState::Ready);
            if (!entry.state.compare_exchange_strong(
                    expected, static_cast<std::uint8_t>(EntryState::Claimed),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            result.path = entry.completion == SharedIoCompletion::Success
                              ? SharedIoDeliveryPath::Gpu
                              : fallback_path();
            result.fallback_reason = entry.failure_reason;
            result.resynced = late;
            retire_claimed_entry(entry);
            if (telemetry_)
                telemetry_->record_delivery(result.uses_fallback(), false);
            return result;
        }

        result.path = fallback_path();
        result.fallback_reason = SharedIoFallbackReason::DeadlineExceeded;
        if (telemetry_)
            telemetry_->record_deadline_miss();
        if (telemetry_)
            telemetry_->record_delivery(result.uses_fallback(), false);
        result.resynced = late;
        return result;
    }

  private:
    enum class EntryState : std::uint8_t { Empty, Writing, Admitted, Completing, Ready, Claimed };
    struct Entry {
        std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(EntryState::Empty)};
        std::uint64_t sequence = 0;
        SharedIoCompletion completion = SharedIoCompletion::Failed;
        SharedIoFallbackReason failure_reason = SharedIoFallbackReason::SubmissionRejected;
    };

    SharedIoDeliveryPath fallback_path() const noexcept {
        switch (contract_.miss_policy) {
        case MissPolicy::CpuFallback:
            return contract_.cpu_fallback_prepared ? SharedIoDeliveryPath::CpuFallback
                                                   : SharedIoDeliveryPath::Silence;
        case MissPolicy::PassthroughDry:
            return SharedIoDeliveryPath::PassthroughDry;
        case MissPolicy::Silence:
            return SharedIoDeliveryPath::Silence;
        }
        return SharedIoDeliveryPath::Silence;
    }

    void resync_admission(std::uint64_t sequence) noexcept {
        auto current = next_admission_sequence_.load(std::memory_order_acquire);
        while (current < sequence &&
               !next_admission_sequence_.compare_exchange_weak(
                   current, sequence, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }

    void discard_ready_entries_before(std::uint64_t sequence) noexcept {
        for (std::uint32_t index = 0; index < capacity_; ++index) {
            Entry& entry = entries_[index];
            auto state = entry.state.load(std::memory_order_acquire);
            if (state != static_cast<std::uint8_t>(EntryState::Ready))
                continue;
            if (entry.sequence >= sequence)
                continue;
            if (!entry.state.compare_exchange_strong(
                    state, static_cast<std::uint8_t>(EntryState::Claimed),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            retire_claimed_entry(entry);
            if (telemetry_) {
                telemetry_->record_resync_drop();
                telemetry_->record_late_completion();
            }
        }
    }

    void retire_claimed_entry(Entry& entry) noexcept {
        const auto remaining = in_flight_.fetch_sub(1, std::memory_order_relaxed) - 1;
#if defined(PULP_GPU_AUDIO_CONTROLLER_TEST_HOOKS)
        if (before_reuse_test_hook_)
            before_reuse_test_hook_(*this, remaining, before_reuse_test_context_);
#endif
        entry.state.store(static_cast<std::uint8_t>(EntryState::Empty), std::memory_order_release);
    }

    std::unique_ptr<Entry[]> entries_;
    SharedIoExecutionContract contract_;
    SharedIoTelemetry* telemetry_ = nullptr;
    std::atomic<std::uint32_t> in_flight_{0};
    std::uint32_t capacity_ = 0;
    std::atomic<std::uint64_t> next_admission_sequence_{0};
    std::uint64_t next_callback_sequence_ = 0;
    bool callback_started_ = false;
    bool prepared_ = false;
#if defined(PULP_GPU_AUDIO_CONTROLLER_TEST_HOOKS)
    BeforePublishTestHook before_publish_test_hook_ = nullptr;
    void* before_publish_test_context_ = nullptr;
    BeforeReuseTestHook before_reuse_test_hook_ = nullptr;
    void* before_reuse_test_context_ = nullptr;
#endif
};

static_assert(noexcept(std::declval<SharedIoExecutionController&>().deliver(0)));
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace pulp::gpu_audio::detail
