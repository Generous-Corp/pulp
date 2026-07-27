#pragma once

#include <pulp/timeline/transaction.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_persistence
 * @{
 */

namespace detail {
class JournalAccess;
}

/// In-memory command-journal quotas.
///
/// A new entry must fit all three ceilings atomically; no automatic journal
/// eviction occurs. checkpoint() releases accounting for the discarded prefix.
struct JournalLimits {
    std::size_t max_transactions = 1024;
    std::size_t max_commands = 8192;
    std::size_t max_retained_bytes = 16 * 1024 * 1024;
};

/// Distinguishes user submissions from generated undo/redo transactions.
enum class JournalEntryKind : std::uint8_t { Ordinary, History };

/// One contiguous revision transition retained by the command journal.
///
/// transaction.expected_revision equals before, after is before plus one, and
/// dirty is the deterministic reduction result for transaction.
struct JournalEntry {
    DocumentRevision before;
    DocumentRevision after;
    Transaction transaction;
    DirtySet dirty;
    JournalEntryKind kind = JournalEntryKind::Ordinary;
};

/// Failure category returned by a durable JournalSink.
enum class JournalSinkError : std::uint8_t {
    Closed,
    IoError,
    InvalidState,
    DurabilityUncertain,
};

/// Synchronous durability boundary used by DocumentSession.
///
/// Implementations are retained by shared ownership. Calls are serialized by
/// the originating session and execute while its writer lock is held, so a sink
/// must not call lock-taking APIs on that session. Methods are noexcept and must
/// translate exceptions or platform failures into JournalSinkError.
class JournalSink {
  public:
    virtual ~JournalSink() = default;

    JournalSink(const JournalSink&) = delete;
    JournalSink& operator=(const JournalSink&) = delete;

    /// Makes a complete transaction entry durable as one batch.
    ///
    /// Returns Ok(true) only after the complete transaction batch is durable.
    /// Ok(false) is a durability failure and permanently rejects later durable
    /// writes from the attached session.
    /// The session writer lock is held during this call, so implementations
    /// must not invoke lock-taking APIs on the originating DocumentSession.
    /// Any result other than Ok(true) permanently rejects later durable writes
    /// from that session.
    virtual runtime::Result<bool, JournalSinkError>
    append_batch(const JournalEntry& entry) noexcept = 0;

    /// Installs a checkpoint before entries through its revision are discarded.
    ///
    /// Durably installs the snapshot before discarding journal entries through
    /// durable_revision. Only Ok(true) acknowledges durability; the session
    /// retains its prior checkpoint on any other result.
    /// The session writer lock is held during this call, so implementations
    /// must not invoke lock-taking APIs on the originating DocumentSession.
    /// Any result other than Ok(true) permanently rejects later durable writes
    /// from that session.
    virtual runtime::Result<bool, JournalSinkError>
    checkpoint(const Project& snapshot, DocumentRevision durable_revision) noexcept = 0;

    /// Validates attachment to an already-durable snapshot and revision.
    ///
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

/// Bounded, replayable in-memory log of contiguous document revisions.
///
/// The journal owns its entries and, after its first append, the checkpoint
/// snapshot needed to validate replay. Public inspection views remain valid
/// until the journal is mutated or destroyed. DocumentSession serializes all
/// mutation; a standalone CommandJournal is not internally synchronized.
class CommandJournal {
  public:
    /// Creates an empty revision-zero journal with fixed quotas.
    explicit CommandJournal(JournalLimits limits) : limits_(limits) {}

    /// Deterministically applies all entries after the supplied checkpoint.
    ///
    /// The checkpoint revision and, when retained, snapshot must match the
    /// journal base. Replay verifies contiguous revisions, writer-scoped
    /// transaction and command monotonicity, model validity, and exact DirtySet
    /// reproduction.
    runtime::Result<Project, TransactionError> replay(const Project& checkpoint,
                                                      DocumentRevision checkpoint_revision) const;

    /// Returns the retained suffix in revision order.
    std::span<const JournalEntry> entries() const noexcept {
        return entries_;
    }
    /// Returns the saturated retained-size accounting for entries.
    std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    /// Returns the total number of command envelopes in retained entries.
    std::size_t command_count() const noexcept {
        return command_count_;
    }
    /// Returns the revision of the checkpoint preceding entries().
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

/// Returns a saturated retained-memory estimate for one journal entry.
std::size_t retained_size(const JournalEntry& entry) noexcept;

/// @}

} // namespace pulp::timeline
