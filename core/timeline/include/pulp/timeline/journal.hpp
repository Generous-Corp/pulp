#pragma once

#include <pulp/timeline/transaction.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline {

namespace detail {
class JournalAccess;
}

struct JournalLimits {
    std::size_t max_transactions = 1024;
    std::size_t max_commands = 8192;
    std::size_t max_retained_bytes = 16 * 1024 * 1024;
};

enum class JournalEntryKind : std::uint8_t { Ordinary, History };

struct JournalEntry {
    DocumentRevision before;
    DocumentRevision after;
    Transaction transaction;
    DirtySet dirty;
    JournalEntryKind kind = JournalEntryKind::Ordinary;
};

enum class JournalSinkError : std::uint8_t {
    Closed,
    IoError,
    InvalidState,
    DurabilityUncertain,
};

class JournalSink {
  public:
    virtual ~JournalSink() = default;

    JournalSink(const JournalSink&) = delete;
    JournalSink& operator=(const JournalSink&) = delete;

    /// Returns Ok(true) only after the complete transaction batch is durable.
    /// Ok(false) is a durability failure and permanently rejects later durable
    /// writes from the attached session.
    /// The session writer lock is held during this call, so implementations
    /// must not invoke lock-taking APIs on the originating DocumentSession.
    /// Any result other than Ok(true) permanently rejects later durable writes
    /// from that session.
    virtual runtime::Result<bool, JournalSinkError>
    append_batch(const JournalEntry& entry) noexcept = 0;

    /// Durably installs the snapshot before discarding journal entries through
    /// durable_revision. Only Ok(true) acknowledges durability; the session
    /// retains its prior checkpoint on any other result.
    /// The session writer lock is held during this call, so implementations
    /// must not invoke lock-taking APIs on the originating DocumentSession.
    /// Any result other than Ok(true) permanently rejects later durable writes
    /// from that session.
    virtual runtime::Result<bool, JournalSinkError>
    checkpoint(const Project& snapshot, DocumentRevision durable_revision) noexcept = 0;

    /// Verifies that an already-durable sink exactly matches a recovered
    /// snapshot and revision before a session attaches to it. Only Ok(true)
    /// permits attachment. This operation must not mutate or truncate durable
    /// state.
    virtual runtime::Result<bool, JournalSinkError> validate_restore(const Project&,
                                                                     DocumentRevision) noexcept {
        return runtime::Result<bool, JournalSinkError>(
            runtime::Err(JournalSinkError::InvalidState));
    }

  protected:
    JournalSink() = default;
};

class CommandJournal {
  public:
    explicit CommandJournal(JournalLimits limits) : limits_(limits) {}

    runtime::Result<Project, TransactionError> replay(const Project& checkpoint,
                                                      DocumentRevision checkpoint_revision) const;

    std::span<const JournalEntry> entries() const noexcept {
        return entries_;
    }
    std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    std::size_t command_count() const noexcept {
        return command_count_;
    }
    DocumentRevision base_revision() const noexcept {
        return base_revision_;
    }

  private:
    struct WriterWatermark {
        WriterId id;
        std::uint64_t transaction = 0;
        std::uint64_t command = 0;
    };
    friend class detail::JournalAccess;
    runtime::Result<bool, TransactionError> preflight(const JournalEntry& entry) const;
    std::optional<Project> prepare_append(const Project& before);
    void append_prepared(JournalEntry entry, std::optional<Project> initial_snapshot) noexcept;
    bool checkpoint(DocumentRevision durable_revision);

    JournalLimits limits_;
    std::vector<JournalEntry> entries_;
    std::size_t retained_bytes_ = 0;
    std::size_t command_count_ = 0;
    DocumentRevision base_revision_{};
    std::optional<Project> base_snapshot_;
    std::vector<WriterWatermark> base_writers_;
};

std::size_t retained_size(const JournalEntry& entry) noexcept;

} // namespace pulp::timeline
