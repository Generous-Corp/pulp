#pragma once

#include "shared_io_trace.hpp"

#include <cstdint>
#include <unordered_map>
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
    bool admit(std::uint64_t request_id, std::uint64_t sequence, std::uint32_t slot,
               std::uint64_t now_ns) {
        if (request_id == 0 || entries_.contains(request_id))
            return false;
        Entry entry;
        entry.record.kind = SharedIoTraceKind::Terminal;
        entry.record.sequence = sequence;
        entry.record.gpu_work_admitted = true;
        entry.record.set(SharedIoTraceStage::Scheduled, now_ns);
        entry.record.set(SharedIoTraceStage::WorkerEntry, now_ns);
        entry.slot = slot;
        entries_.emplace(request_id, entry);
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
    std::vector<SharedIoTraceRecord> completed_;
};

// Persistent worker-side ownership for one staged async transport. This layer
// keeps request IDs and output slots alive across submit/poll callbacks; it is
// deliberately independent of GpuConvolver until the callback state machine is
// ready to preserve the existing blocking behavior.
class StagedAsyncPendingState {
  public:
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

  private:
    std::vector<bool> slots_;
    std::unordered_map<std::uint64_t, Pending> pending_;
};

} // namespace pulp::gpu_audio::detail
