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

enum class ControlSecurityOutcome : std::uint8_t {
    Accepted,
    Denied,
    Revoked,
    Expired,
};

/// Metadata-only security event. Bootstrap secrets, credentials, consent text,
/// and operation payload values have no fields in this record.
struct ControlSecurityAuditEntry {
    std::uint64_t sequence = 0;
    std::string action;
    std::string broker_id;
    std::string peer_fingerprint;
    std::string client_id;
    std::string registration_id;
    std::string grant_id;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string capability_id;
    ControlSecurityOutcome outcome = ControlSecurityOutcome::Denied;
    std::string reason;
};

class ControlSecurityAuditLog {
public:
    explicit ControlSecurityAuditLog(std::size_t capacity = 512)
        : capacity_(std::max<std::size_t>(capacity, 1)) {}

    void append(ControlSecurityAuditEntry entry) {
        std::lock_guard lock(mutex_);
        entry.sequence = next_sequence_++;
        if (entries_.size() == capacity_)
            entries_.pop_front();
        entries_.push_back(std::move(entry));
    }

    std::vector<ControlSecurityAuditEntry> snapshot() const {
        std::lock_guard lock(mutex_);
        return {entries_.begin(), entries_.end()};
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<ControlSecurityAuditEntry> entries_;
    std::uint64_t next_sequence_ = 1;
};

} // namespace pulp::inspect
