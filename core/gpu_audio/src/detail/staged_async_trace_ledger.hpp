#pragma once

#include "shared_io_trace.hpp"

#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulp::gpu_audio::detail {

// Worker-owned bookkeeping for the legacy staged async provider. It binds the
// provider request id to the audio sequence and output slot before submission;
// completion is accepted exactly once and produces an authenticated trace
// record with direct submit/completion timestamps. The audio callback never
// touches this type.
class StagedAsyncTraceLedger {
  public:
    enum class CompletionStatus : std::uint8_t { Success, Expired, Failed };
    explicit StagedAsyncTraceLedger(std::uint64_t generation = 1)
        : generation_(generation == 0 ? 1 : generation) {}

    bool admit(std::uint64_t request_id, std::uint64_t sequence, std::uint32_t slot,
               std::uint64_t now_ns) {
        if (request_id == 0 || entries_.contains(request_id) || sequences_.contains(sequence))
            return false;
        Entry entry;
        entry.record.kind = SharedIoTraceKind::Terminal;
        entry.record.generation = generation_;
        entry.record.sequence = sequence;
        entry.record.gpu_work_admitted = true;
        entry.record.set(SharedIoTraceStage::Scheduled, now_ns);
        entry.record.set(SharedIoTraceStage::WorkerEntry, now_ns);
        entry.slot = slot;
        entries_.emplace(request_id, entry);
        sequences_.insert(sequence);
        return true;
    }

    bool submitted(std::uint64_t request_id, std::uint64_t now_ns) {
        auto it = entries_.find(request_id);
        if (it == entries_.end() || it->second.submitted)
            return false;
        auto& entry = it->second;
        entry.record.set(SharedIoTraceStage::EncodeBegin, entry.record.cpu_ns[1]);
        entry.record.set(SharedIoTraceStage::EncodeEnd, now_ns);
        entry.record.set(SharedIoTraceStage::SubmitBegin, now_ns);
        entry.record.set(SharedIoTraceStage::SubmitEnd, now_ns);
        entry.submitted = true;
        return true;
    }

    bool complete(std::uint64_t request_id, CompletionStatus status, std::uint64_t now_ns) {
        auto it = entries_.find(request_id);
        if (it == entries_.end() || !it->second.submitted)
            return false;
        auto entry = it->second;
        entries_.erase(it);
        sequences_.erase(entry.record.sequence);
        entry.record.set(SharedIoTraceStage::CompletionObserved, now_ns);
        if (status == CompletionStatus::Success) {
            entry.record.gpu_terminal = SharedIoGpuTerminalDisposition::CompletedAccepted;
            entry.record.outcome = SharedIoTraceOutcome::Success;
        } else if (status == CompletionStatus::Expired) {
            entry.record.gpu_terminal = SharedIoGpuTerminalDisposition::LateRejected;
            entry.record.outcome = SharedIoTraceOutcome::LateRejected;
            entry.record.gpu_reason = SharedIoFallbackReason::Timeout;
        } else {
            entry.record.gpu_terminal = SharedIoGpuTerminalDisposition::ProviderFailed;
            entry.record.outcome = SharedIoTraceOutcome::CompletionFailed;
            entry.record.gpu_reason = SharedIoFallbackReason::CompletionFailed;
        }
        completed_.push_back(entry.record);
        return true;
    }

    // Roll back an admission that could not be attached to a provider slot.
    // No terminal record is emitted for work that was never submitted.
    bool cancel_admission(std::uint64_t request_id) noexcept {
        auto it = entries_.find(request_id);
        if (it == entries_.end() || it->second.submitted)
            return false;
        sequences_.erase(it->second.record.sequence);
        entries_.erase(it);
        return true;
    }

    // Close an admitted request that cannot reach its normal callback. This
    // preserves exactly-once terminal accounting for provider-side failures.
    bool abandon(std::uint64_t request_id, std::uint64_t now_ns) {
        auto it = entries_.find(request_id);
        if (it == entries_.end())
            return false;
        auto entry = it->second;
        entries_.erase(it);
        sequences_.erase(entry.record.sequence);
        entry.record.set(SharedIoTraceStage::CompletionObserved, now_ns);
        entry.record.gpu_terminal = SharedIoGpuTerminalDisposition::CancelledTeardown;
        entry.record.outcome = SharedIoTraceOutcome::Cancelled;
        completed_.push_back(entry.record);
        return true;
    }

    std::vector<SharedIoTraceRecord> take_completed() {
        std::vector<SharedIoTraceRecord> result;
        result.swap(completed_);
        return result;
    }

    bool empty() const noexcept {
        return entries_.empty();
    }

  private:
    struct Entry {
        SharedIoTraceRecord record;
        std::uint32_t slot = 0;
        bool submitted = false;
    };
    std::unordered_map<std::uint64_t, Entry> entries_;
    // A sequence identifies one admitted audio block. Rejecting duplicate
    // sequence admission makes the terminal-accounting invariant explicit and
    // prevents a producer from emitting two dispositions for one block.
    std::unordered_set<std::uint64_t> sequences_;
    std::vector<SharedIoTraceRecord> completed_;
    std::uint64_t generation_ = 1;
};

// Persistent worker-side ownership for one staged async transport. This layer
// keeps request IDs and output slots alive across submit/poll callbacks; it is
// deliberately independent of GpuConvolver until the callback state machine is
// ready to preserve the existing blocking behavior.
class StagedAsyncPendingState {
  public:
    enum class CallbackStatus : std::uint8_t { Success, Expired, Failed };
    struct Pending {
        std::uint64_t request_id = 0;
        std::uint64_t sequence = 0;
        std::uint32_t slot = 0;
        std::uint64_t deadline_ns = 0;
        bool submitted = false;
    };

    explicit StagedAsyncPendingState(std::size_t slot_count) : slots_(slot_count, false) {}

    bool admit(std::uint64_t request_id, std::uint64_t sequence, std::uint32_t slot,
               std::uint64_t deadline_ns) {
        if (request_id == 0 || slot >= slots_.size() || slots_[slot] ||
            pending_.contains(request_id))
            return false;
        slots_[slot] = true;
        pending_.emplace(request_id, Pending{request_id, sequence, slot, deadline_ns, false});
        return true;
    }

    bool mark_submitted(std::uint64_t request_id) {
        auto it = pending_.find(request_id);
        if (it == pending_.end() || it->second.submitted)
            return false;
        it->second.submitted = true;
        return true;
    }

    bool complete(std::uint64_t request_id) {
        auto it = pending_.find(request_id);
        if (it == pending_.end() || !it->second.submitted)
            return false;
        slots_[it->second.slot] = false;
        pending_.erase(it);
        return true;
    }

    bool cancel(std::uint64_t request_id) noexcept {
        auto it = pending_.find(request_id);
        if (it == pending_.end() || it->second.submitted)
            return false;
        slots_[it->second.slot] = false;
        pending_.erase(it);
        return true;
    }

    bool abandon(std::uint64_t request_id) noexcept {
        auto it = pending_.find(request_id);
        if (it == pending_.end())
            return false;
        slots_[it->second.slot] = false;
        pending_.erase(it);
        return true;
    }

    // Adapter boundary for GpuCompute::ReadbackCallback. The callback owner
    // maps ReadbackStatus to this enum, then releases the slot exactly once.
    bool on_callback(std::uint64_t request_id, CallbackStatus status) {
        (void)status; // disposition mapping is owned by the trace ledger
        return complete(request_id);
    }

    std::vector<Pending> expire(std::uint64_t now_ns) {
        std::vector<Pending> expired;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.submitted && it->second.deadline_ns <= now_ns) {
                slots_[it->second.slot] = false;
                expired.push_back(it->second);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return expired;
    }

    std::size_t size() const noexcept {
        return pending_.size();
    }

    bool empty() const noexcept { return pending_.empty(); }

    bool slot_occupied(std::uint32_t slot) const noexcept {
        return slot < slots_.size() && slots_[slot];
    }

  private:
    std::vector<bool> slots_;
    std::unordered_map<std::uint64_t, Pending> pending_;
};

// Private trial boundary shared by the staged provider adapter and the future
// GpuConvolver trial path. It owns request/sequence/slot admission as one
// transaction, while keeping terminal records in the authenticated ledger.
// This is deliberately unused by the default blocking path.
class StagedAsyncTrialState {
  public:
    explicit StagedAsyncTrialState(std::size_t slots, std::uint64_t generation = 1)
        : ledger_(generation), pending_(slots) {}

    bool admit(std::uint64_t request_id, std::uint64_t sequence, std::uint32_t slot,
               std::uint64_t deadline_ns, std::uint64_t now_ns) {
        if (!ledger_.admit(request_id, sequence, slot, now_ns))
            return false;
        if (!pending_.admit(request_id, sequence, slot, deadline_ns)) {
            (void)ledger_.cancel_admission(request_id);
            return false;
        }
        return true;
    }

    bool submitted(std::uint64_t request_id, std::uint64_t now_ns) {
        if (!pending_.mark_submitted(request_id))
            return false;
        if (!ledger_.submitted(request_id, now_ns)) {
            (void)pending_.abandon(request_id);
            (void)ledger_.cancel_admission(request_id);
            return false;
        }
        return true;
    }

    bool complete(std::uint64_t request_id, StagedAsyncTraceLedger::CompletionStatus status,
                  std::uint64_t now_ns) {
        if (!ledger_.complete(request_id, status, now_ns)) {
            // If the provider callback arrived after local ownership was lost,
            // close any remaining slot without fabricating a second record.
            (void)pending_.abandon(request_id);
            return false;
        }
        (void)pending_.abandon(request_id);
        return true;
    }

    bool abandon(std::uint64_t request_id, std::uint64_t now_ns) {
        const auto closed = ledger_.abandon(request_id, now_ns);
        (void)pending_.abandon(request_id);
        return closed;
    }

    bool slot_occupied(std::uint32_t slot) const noexcept {
        return pending_.slot_occupied(slot);
    }
    std::size_t pending_count() const noexcept { return pending_.size(); }
    // Records may only be drained after the worker has stopped admitting or
    // polling requests.  Returning this as an explicit predicate keeps the
    // quiescent ownership requirement at the producer boundary instead of
    // making callers infer it from an empty record vector.
    bool quiescent() const noexcept { return pending_.empty(); }
    std::vector<SharedIoTraceRecord> take_completed() { return ledger_.take_completed(); }

  private:
    StagedAsyncTraceLedger ledger_;
    StagedAsyncPendingState pending_;
};

inline StagedAsyncPendingState::CallbackStatus
staged_async_callback_status(render::GpuCompute::ReadbackStatus status) noexcept {
    switch (status) {
    case render::GpuCompute::ReadbackStatus::Success:
        return StagedAsyncPendingState::CallbackStatus::Success;
    case render::GpuCompute::ReadbackStatus::Expired:
        return StagedAsyncPendingState::CallbackStatus::Expired;
    case render::GpuCompute::ReadbackStatus::Failed:
        return StagedAsyncPendingState::CallbackStatus::Failed;
    }
    return StagedAsyncPendingState::CallbackStatus::Failed;
}

// Non-RT adapter harness used by the staged campaign before attachment to
// GpuConvolver. It proves request assignment and callback routing against the
// real GpuCompute API while leaving production process_block untouched.
class StagedAsyncRequestHarness {
  public:
    enum class OutputDisposition : std::uint8_t { GpuDelivered, CpuFallback, StaleRejected };
    explicit StagedAsyncRequestHarness(render::GpuCompute& compute, std::size_t slots = 1)
        : compute_(compute), pending_(slots) {}

    std::uint64_t submit(const float* input, float* output, std::uint32_t fft_size,
                         std::uint32_t channels, std::uint64_t sequence, std::uint32_t slot,
                         std::chrono::microseconds deadline) {
        auto request_holder = std::make_shared<std::uint64_t>(0);
        const auto request = compute_.convolve_batch_async(
            input, output, fft_size, channels, deadline,
            [this, request_holder](const render::GpuCompute::ReadbackResult& result) {
                (void)handoff_result(*request_holder, staged_async_callback_status(result.status),
                                     result.status == render::GpuCompute::ReadbackStatus::Success);
            });
        if (request == 0)
            return 0;
        *request_holder = request;
        const auto now = now_ns();
        if (!pending_.admit(request, sequence, slot,
                            now + static_cast<std::uint64_t>(deadline.count()) * 1000u) ||
            !pending_.mark_submitted(request))
            return 0;
        return request;
    }

    std::size_t poll() noexcept {
        return compute_.poll_readbacks();
    }
    std::size_t pending_count() const noexcept {
        return pending_.size();
    }
    bool slot_occupied(std::uint32_t slot) const noexcept {
        return pending_.slot_occupied(slot);
    }
    std::size_t expire(std::uint64_t now_ns) {
        return pending_.expire(now_ns).size();
    }

    OutputDisposition handoff_result(std::uint64_t request_id,
                                     StagedAsyncPendingState::CallbackStatus status,
                                     bool output_ready) {
        if (!pending_.on_callback(request_id, status))
            return OutputDisposition::StaleRejected;
        if (status == StagedAsyncPendingState::CallbackStatus::Success && output_ready)
            return OutputDisposition::GpuDelivered;
        return OutputDisposition::CpuFallback;
    }

  private:
    static std::uint64_t now_ns() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    render::GpuCompute& compute_;
    StagedAsyncPendingState pending_;
};

} // namespace pulp::gpu_audio::detail
