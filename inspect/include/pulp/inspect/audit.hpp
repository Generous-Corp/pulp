#pragma once

#include <pulp/inspect/capabilities.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {

enum class InspectorAuditOutcome : std::uint8_t {
    Denied,
    Applied,
    Rejected,
};

/// Metadata-only record for a mutating inspector request. Request parameters
/// are deliberately absent so credentials, source content, and authoring data
/// cannot enter the audit buffer.
struct InspectorAuditEntry {
    std::uint64_t sequence = 0;
    std::int64_t request_id = 0;
    std::string session_id;
    std::string instance_id;
    std::string client_id;
    std::string method;
    InspectorCapability capability = InspectorCapability::Unavailable;
    InspectorAuditOutcome outcome = InspectorAuditOutcome::Rejected;
    std::string error_code;
};

class InspectorAuditLog {
  public:
    explicit InspectorAuditLog(std::size_t capacity = 256)
        : capacity_(std::max<std::size_t>(capacity, 1)) {}

    void append(InspectorAuditEntry entry) {
        std::lock_guard lock(mutex_);
        entry.sequence = next_sequence_++;
        if (entries_.size() == capacity_)
            entries_.pop_front();
        entries_.push_back(std::move(entry));
    }
    std::vector<InspectorAuditEntry> snapshot() const {
        std::lock_guard lock(mutex_);
        return {entries_.begin(), entries_.end()};
    }
    std::size_t capacity() const {
        return capacity_;
    }

  private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<InspectorAuditEntry> entries_;
    std::uint64_t next_sequence_ = 1;
};

} // namespace pulp::inspect
