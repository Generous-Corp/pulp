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

} // namespace pulp::gpu_audio::detail
