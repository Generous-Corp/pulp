#pragma once

#include <pulp/timeline/journal.hpp>
#include <pulp/timeline/undo.hpp>

#include <atomic>
#include <memory>

namespace pulp::timeline {

/** @addtogroup timeline_editing
 * @{
 */

namespace detail {
class DocumentSessionPreviewAccess;
class WriterTokenTestAccess;
}

/// Resource ceilings for one mutable document session.
///
/// Limits are fixed when the session is created. Journal and undo accounting
/// use saturated retained-size estimates; writer and result limits bound the
/// corresponding in-memory tables.
struct SessionLimits {
    JournalLimits journal;
    UndoLimits undo;
    std::size_t max_writers = 32;
    std::size_t max_cached_results = 256;
};

/// Move-only authority for submitting transactions to one DocumentSession.
///
/// A token is bound to the session that created it and remains valid until that
/// session is destroyed. Its three allocation methods are safe to call
/// concurrently and produce monotonically increasing, writer-scoped IDs.
/// Exhaustion produces an invalid ID rather than wrapping.
class WriterToken {
  public:
    /// Constructs an invalid token that cannot authorize submissions.
    WriterToken() = default;
    WriterToken(const WriterToken&) = delete;
    WriterToken& operator=(const WriterToken&) = delete;
    /// Transfers writer authority and invalidates other.
    WriterToken(WriterToken&& other) noexcept;
    /// Replaces this authority with other and invalidates other.
    WriterToken& operator=(WriterToken&& other) noexcept;

    /// Returns the writer identity carried by this token.
    WriterId id() const noexcept {
        return id_;
    }
    /// Allocates the next transaction ID, or an invalid ID after exhaustion.
    TransactionId allocate_transaction_id() noexcept;
    /// Allocates the next command ID, or an invalid ID after exhaustion.
    CommandId allocate_command_id() noexcept;
    /// Allocates the next undo-group ID, or an invalid ID after exhaustion.
    UndoGroupId allocate_undo_group_id() noexcept;

  private:
    friend class DocumentSession;
    friend class detail::WriterTokenTestAccess;
    WriterId id_;
    std::uint64_t owner_nonce_ = 0;
    std::atomic<std::uint64_t> next_transaction_{1};
    std::atomic<std::uint64_t> next_command_{1};
    std::atomic<std::uint64_t> next_undo_group_{1};
};

/// Atomically observed immutable project snapshot and its matching revision.
///
/// The shared snapshot remains valid independently of later commits and of the
/// session lifetime.
struct DocumentView {
    std::shared_ptr<const Project> snapshot;
    DocumentRevision revision;
};

/// Thread-safe owner of a revisioned Timeline document.
///
/// Reads publish immutable snapshots atomically. Submissions, undo/redo,
/// checkpointing, and writer registration are serialized internally and must
/// not run on a real-time audio thread. When a JournalSink is attached, a
/// revision is published only after the sink acknowledges the whole journal
/// entry as durable. A failed durability acknowledgement permanently rejects
/// subsequent durable writes from this session.
class DocumentSession {
  public:
    /// Creates a session at revision zero without an external durable sink.
    ///
    /// @param initial Initial immutable document value.
    /// @param limits Fixed resource ceilings for the session.
    /// @return The session, or WriterLimit/SequenceExhausted when it cannot
    ///         allocate the required session state.
    static runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
    create(Project initial, SessionLimits limits = {});
    /// Creates a session at revision zero and initializes a durable sink.
    ///
    /// The sink is checkpointed and then validated before attachment. The
    /// session retains shared ownership of it and invokes it while holding the
    /// session writer lock.
    ///
    /// @return The session, or JournalDurability when initialization or
    ///         validation is not acknowledged.
    static runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
    create(Project initial, SessionLimits limits, std::shared_ptr<JournalSink> journal_sink);
    /// Restores a session from a checkpoint already present in a durable sink.
    ///
    /// This operation validates exact snapshot and revision agreement without
    /// rewriting the sink. The session retains shared ownership of the sink.
    ///
    /// @return The restored session, or JournalDurability when the sink does
    ///         not validate the supplied checkpoint.
    static runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
    restore(Project checkpoint, DocumentRevision checkpoint_revision, SessionLimits limits,
            std::shared_ptr<JournalSink> journal_sink);
    /// Releases the session and its retained sink.
    ///
    /// The caller must ensure no member call is in progress during destruction.
    ~DocumentSession();

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    /// Registers a distinct writer until max_writers or ID space is exhausted.
    runtime::Result<WriterToken, TransactionError> register_writer();
    /// Returns one atomic snapshot/revision observation without taking the writer lock.
    DocumentView current() const noexcept;
    /// Returns the currently published immutable snapshot.
    std::shared_ptr<const Project> snapshot() const noexcept;
    /// Returns the revision paired with the currently published snapshot.
    DocumentRevision revision() const noexcept;
    /// Applies and, when configured, durably journals one transaction.
    ///
    /// The token must belong to this session and match the transaction writer.
    /// The transaction must target the current revision and use strictly
    /// increasing writer-scoped IDs. Equivalent retries within the result cache
    /// return their original CommitResult without applying twice. Successful
    /// commits advance the revision by one and invalidate the redo stack.
    ///
    /// @return The published commit, or a TransactionError describing identity,
    ///         revision, gesture, quota, model, or durability rejection.
    runtime::Result<CommitResult, TransactionError> submit(WriterToken& writer,
                                                           Transaction transaction);
    /// Applies the newest closed undo group as a durable history transaction.
    ///
    /// The supplied token determines the IDs of the generated history
    /// transaction. Undo is rejected while a gesture is open.
    runtime::Result<CommitResult, TransactionError> undo(WriterToken& writer);
    /// Reapplies the newest redo group as a durable history transaction.
    ///
    /// The supplied token determines the IDs of the generated history
    /// transaction. Redo is rejected while a gesture is open.
    runtime::Result<CommitResult, TransactionError> redo(WriterToken& writer);
    /// Reports whether a closed undo group is currently available.
    bool can_undo() const noexcept;
    /// Reports whether a redo group is currently available.
    bool can_redo() const noexcept;
    /// Returns a locked copy of the bounded in-memory command journal.
    CommandJournal journal() const;
    /// Advances the journal checkpoint through an existing revision.
    ///
    /// With a sink, the reconstructed checkpoint is installed durably before
    /// in-memory entries are discarded. Failure leaves the prior in-memory
    /// checkpoint intact; a sink failure also disables later durable writes.
    bool checkpoint(DocumentRevision durable_revision);

  private:
    friend class detail::DocumentSessionPreviewAccess;
    enum class SinkAttachment : std::uint8_t { Initialize, Restore };

    static runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
    create_impl(Project checkpoint, DocumentRevision checkpoint_revision, SessionLimits limits,
                std::shared_ptr<JournalSink> journal_sink, SinkAttachment attachment);

    struct Impl;
    explicit DocumentSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// @}

} // namespace pulp::timeline
