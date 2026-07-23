#pragma once

#include <pulp/timeline/journal.hpp>
#include <pulp/timeline/undo.hpp>

#include <atomic>
#include <memory>

namespace pulp::timeline {

namespace detail {
class WriterTokenTestAccess;
}

struct SessionLimits {
    JournalLimits journal;
    UndoLimits undo;
    std::size_t max_writers = 32;
    std::size_t max_cached_results = 256;
};

class WriterToken {
  public:
    WriterToken() = default;
    WriterToken(const WriterToken&) = delete;
    WriterToken& operator=(const WriterToken&) = delete;
    WriterToken(WriterToken&& other) noexcept;
    WriterToken& operator=(WriterToken&& other) noexcept;

    WriterId id() const noexcept {
        return id_;
    }
    TransactionId allocate_transaction_id() noexcept;
    CommandId allocate_command_id() noexcept;
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

struct DocumentView {
    std::shared_ptr<const Project> snapshot;
    DocumentRevision revision;
};

class DocumentSession {
  public:
    static runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
    create(Project initial, SessionLimits limits = {});
    static runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
    create(Project initial, SessionLimits limits, std::shared_ptr<JournalSink> journal_sink);
    ~DocumentSession();

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    runtime::Result<WriterToken, TransactionError> register_writer();
    DocumentView current() const noexcept;
    std::shared_ptr<const Project> snapshot() const noexcept;
    DocumentRevision revision() const noexcept;
    runtime::Result<CommitResult, TransactionError> submit(WriterToken& writer,
                                                           Transaction transaction);
    runtime::Result<CommitResult, TransactionError> undo(WriterToken& writer);
    runtime::Result<CommitResult, TransactionError> redo(WriterToken& writer);
    bool can_undo() const noexcept;
    bool can_redo() const noexcept;
    CommandJournal journal() const;
    bool checkpoint(DocumentRevision durable_revision);

  private:
    struct Impl;
    explicit DocumentSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::timeline
