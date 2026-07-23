#include <pulp/timeline/document_session.hpp>

#include "journal_internal.hpp"
#include "session_nonce_test_access.hpp"
#include "transaction_internal.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace pulp::timeline {
namespace {

std::atomic<std::uint64_t> g_next_session_nonce{1};

struct WriterState {
    WriterId id;
    std::uint64_t transaction_watermark = 0;
    std::uint64_t command_watermark = 0;
};

struct CachedCommit {
    Transaction transaction;
    CommitResult result;
};

struct UndoRecord {
    WriterId writer;
    std::optional<UndoGroupId> group;
    std::vector<Command> forward;
    std::vector<Command> inverse;
    std::size_t retained_bytes = 0;
    bool closed = true;
};

struct OpenGesture {
    WriterId writer;
    UndoGroupId group;
};

enum class CommitKind : std::uint8_t { Ordinary, History };

struct PublishedState {
    std::shared_ptr<const Project> snapshot;
    DocumentRevision revision;
};

TransactionError error(ConflictCode code, const Transaction& transaction,
                       DocumentRevision current = {}, CommandId command = {}) {
    TransactionError result;
    result.code = code;
    result.transaction = transaction.id;
    result.command = command;
    result.expected_revision = transaction.expected_revision;
    result.current_revision = current;
    return result;
}

template <typename T> runtime::Result<T, TransactionError> failure(TransactionError value) {
    return runtime::Result<T, TransactionError>(runtime::Err(std::move(value)));
}

std::size_t saturated_add(std::size_t lhs, std::size_t rhs) noexcept {
    return rhs > std::numeric_limits<std::size_t>::max() - lhs
               ? std::numeric_limits<std::size_t>::max()
               : lhs + rhs;
}

} // namespace

struct DocumentSession::Impl {
    explicit Impl(Project project, SessionLimits session_limits, std::uint64_t nonce,
                  std::shared_ptr<JournalSink> sink)
        : published(std::make_shared<const PublishedState>(
              PublishedState{std::make_shared<const Project>(std::move(project)), {}})),
          journal(session_limits.journal), limits(session_limits), session_nonce(nonce),
          journal_sink(std::move(sink)) {
        cache.reserve(limits.max_cached_results);
        writers.reserve(limits.max_writers);
    }

    mutable std::mutex mutex;
    std::shared_ptr<const PublishedState> published;
    CommandJournal journal;
    SessionLimits limits;
    std::uint64_t session_nonce = 0;
    std::shared_ptr<JournalSink> journal_sink;
    bool journal_sink_failed = false;
    std::uint64_t next_writer = 1;
    std::vector<WriterState> writers;
    std::vector<CachedCommit> cache;
    std::deque<UndoRecord> undo;
    std::deque<UndoRecord> redo;
    std::size_t undo_bytes = 0;
    std::size_t redo_bytes = 0;
    std::optional<OpenGesture> open_gesture;

    WriterState* find_writer(WriterId id) noexcept {
        const auto found = std::find_if(writers.begin(), writers.end(),
                                        [&](const WriterState& value) { return value.id == id; });
        return found == writers.end() ? nullptr : &*found;
    }

    runtime::Result<CommitResult, TransactionError>
    commit_locked(Transaction transaction, bool record_undo, bool clear_redo, CommitKind kind) {
        const auto current = std::atomic_load_explicit(&published, std::memory_order_relaxed);
        const auto current_revision = current->revision;
        auto* writer = find_writer(transaction.id.writer);
        if (!writer || !transaction.id.valid())
            return failure<CommitResult>(
                error(ConflictCode::InvalidIdentifier, transaction, current_revision));

        const auto cached = std::find_if(cache.begin(), cache.end(), [&](const CachedCommit& item) {
            return item.transaction.id == transaction.id;
        });
        if (cached != cache.end()) {
            if (equivalent(cached->transaction, transaction))
                return runtime::Result<CommitResult, TransactionError>(runtime::Ok(cached->result));
            return failure<CommitResult>(
                error(ConflictCode::TransactionIdCollision, transaction, current_revision));
        }
        if (transaction.id.sequence <= writer->transaction_watermark)
            return failure<CommitResult>(
                error(ConflictCode::AlreadyAppliedResultExpired, transaction, current_revision));
        if (journal_sink_failed)
            return failure<CommitResult>(
                error(ConflictCode::JournalDurability, transaction, current_revision));
        if (transaction.expected_revision != current_revision)
            return failure<CommitResult>(
                error(ConflictCode::StaleRevision, transaction, current_revision));
        if (current_revision.value == std::numeric_limits<std::uint64_t>::max())
            return failure<CommitResult>(
                error(ConflictCode::SequenceExhausted, transaction, current_revision));
        if (transaction.undo_group && (!transaction.undo_group->valid() ||
                                       transaction.undo_group->writer != transaction.id.writer))
            return failure<CommitResult>(
                error(ConflictCode::InvalidIdentifier, transaction, current_revision));
        if (transaction.gesture_phase != GesturePhase::Single && !transaction.undo_group)
            return failure<CommitResult>(
                error(ConflictCode::GestureState, transaction, current_revision));

        switch (transaction.gesture_phase) {
        case GesturePhase::Single:
        case GesturePhase::Begin:
            if (open_gesture)
                return failure<CommitResult>(
                    error(ConflictCode::GestureState, transaction, current_revision));
            break;
        case GesturePhase::Update:
        case GesturePhase::End:
            if (!open_gesture || !transaction.undo_group ||
                open_gesture->writer != transaction.id.writer ||
                open_gesture->group != *transaction.undo_group)
                return failure<CommitResult>(
                    error(ConflictCode::GestureState, transaction, current_revision));
            break;
        default:
            return failure<CommitResult>(
                error(ConflictCode::GestureState, transaction, current_revision));
        }

        std::uint64_t previous_command = writer->command_watermark;
        for (const auto& envelope : transaction.commands) {
            if (envelope.id.writer != transaction.id.writer ||
                envelope.id.sequence <= previous_command)
                return failure<CommitResult>(error(ConflictCode::CommandIdCollision, transaction,
                                                   current_revision, envelope.id));
            previous_command = envelope.id.sequence;
        }

        auto reduced = detail::reduce_transaction(*current->snapshot, transaction,
                                                  kind == CommitKind::History);
        if (!reduced) {
            auto failure_value = reduced.error();
            failure_value.current_revision = current_revision;
            return failure<CommitResult>(std::move(failure_value));
        }

        UndoRecord candidate;
        if (record_undo) {
            candidate.writer = transaction.id.writer;
            candidate.group = transaction.undo_group;
            candidate.closed = transaction.gesture_phase == GesturePhase::Single ||
                               transaction.gesture_phase == GesturePhase::End;
            for (const auto& envelope : transaction.commands)
                candidate.forward.push_back(envelope.command);
            candidate.inverse = reduced->inverses;
            candidate.retained_bytes =
                saturated_add(retained_size(candidate.forward), retained_size(candidate.inverse));

            const bool coalesces = candidate.group && !undo.empty() &&
                                   undo.back().group == candidate.group &&
                                   undo.back().writer == candidate.writer && !undo.back().closed;
            auto projected_groups = undo.size() + (coalesces ? 0 : 1);
            auto projected_bytes = saturated_add(undo_bytes, candidate.retained_bytes);
            std::size_t evictable_groups = 0;
            std::size_t evictable_bytes = 0;
            while ((projected_groups - evictable_groups > limits.undo.max_groups ||
                    projected_bytes - std::min(projected_bytes, evictable_bytes) >
                        limits.undo.max_retained_bytes) &&
                   evictable_groups < undo.size() && undo[evictable_groups].closed) {
                evictable_bytes =
                    saturated_add(evictable_bytes, undo[evictable_groups].retained_bytes);
                ++evictable_groups;
            }
            if (projected_groups - evictable_groups > limits.undo.max_groups ||
                projected_bytes - std::min(projected_bytes, evictable_bytes) >
                    limits.undo.max_retained_bytes)
                return failure<CommitResult>(
                    error(ConflictCode::UndoFull, transaction, current_revision));
        }

        const auto next_revision = DocumentRevision{current_revision.value + 1};
        JournalEntry journal_entry{current_revision, next_revision, transaction, reduced->dirty,
                                   kind == CommitKind::History ? JournalEntryKind::History
                                                               : JournalEntryKind::Ordinary};
        auto journal_preflight = detail::JournalAccess::preflight(journal, journal_entry);
        if (!journal_preflight)
            return failure<CommitResult>(journal_preflight.error());

        auto published = std::make_shared<const Project>(std::move(reduced).value().project);
        auto published_state =
            std::make_shared<const PublishedState>(PublishedState{published, next_revision});
        auto initial_snapshot = detail::JournalAccess::prepare_append(journal, *current->snapshot);
        if (journal_sink) {
            auto durable = journal_sink->append_batch(journal_entry);
            if (!durable) {
                journal_sink_failed = true;
                return failure<CommitResult>(
                    error(ConflictCode::JournalDurability, transaction, current_revision));
            }
        }
        detail::JournalAccess::append_prepared(journal, std::move(journal_entry),
                                               std::move(initial_snapshot));
        std::atomic_store_explicit(&this->published, std::move(published_state),
                                   std::memory_order_release);
        writer->transaction_watermark = transaction.id.sequence;
        writer->command_watermark = previous_command;

        CommitResult result{published, next_revision, reduced->dirty, {}};
        result.applied_commands.reserve(transaction.commands.size());
        for (const auto& envelope : transaction.commands)
            result.applied_commands.push_back(envelope.id);

        if (record_undo) {
            const bool will_coalesce =
                candidate.group && !undo.empty() && undo.back().group == candidate.group &&
                undo.back().writer == candidate.writer && !undo.back().closed;
            while ((!will_coalesce && !undo.empty() && undo.size() >= limits.undo.max_groups) ||
                   (!undo.empty() && saturated_add(undo_bytes, candidate.retained_bytes) >
                                         limits.undo.max_retained_bytes)) {
                if (!undo.front().closed)
                    break;
                undo_bytes -= undo.front().retained_bytes;
                undo.pop_front();
            }
            if (will_coalesce) {
                auto& group = undo.back();
                group.forward.insert(group.forward.end(), candidate.forward.begin(),
                                     candidate.forward.end());
                group.inverse.insert(group.inverse.begin(), candidate.inverse.begin(),
                                     candidate.inverse.end());
                group.retained_bytes =
                    saturated_add(group.retained_bytes, candidate.retained_bytes);
                group.closed = candidate.closed;
                undo_bytes = saturated_add(undo_bytes, candidate.retained_bytes);
            } else {
                undo_bytes = saturated_add(undo_bytes, candidate.retained_bytes);
                undo.push_back(std::move(candidate));
            }
            if (transaction.gesture_phase == GesturePhase::Begin)
                open_gesture = OpenGesture{transaction.id.writer, *transaction.undo_group};
            else if (transaction.gesture_phase == GesturePhase::End)
                open_gesture.reset();
            if (clear_redo) {
                redo.clear();
                redo_bytes = 0;
            }
        }

        if (limits.max_cached_results > 0) {
            if (cache.size() == limits.max_cached_results)
                cache.erase(cache.begin());
            cache.push_back({transaction, result});
        }
        return runtime::Result<CommitResult, TransactionError>(runtime::Ok(std::move(result)));
    }

    Transaction make_history_transaction(WriterToken& writer, std::span<const Command> commands) {
        Transaction transaction;
        transaction.id = writer.allocate_transaction_id();
        transaction.expected_revision =
            std::atomic_load_explicit(&published, std::memory_order_relaxed)->revision;
        transaction.commands.reserve(commands.size());
        for (const auto& command : commands)
            transaction.commands.push_back({writer.allocate_command_id(), command});
        return transaction;
    }
};

std::uint64_t allocate_sequence(std::atomic<std::uint64_t>& next) noexcept {
    auto current = next.load(std::memory_order_relaxed);
    while (current != 0 && current != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                       std::memory_order_relaxed))
            return current;
    }
    return 0;
}

TransactionId WriterToken::allocate_transaction_id() noexcept {
    return {id_, allocate_sequence(next_transaction_)};
}

CommandId WriterToken::allocate_command_id() noexcept {
    return {id_, allocate_sequence(next_command_)};
}

UndoGroupId WriterToken::allocate_undo_group_id() noexcept {
    return {id_, allocate_sequence(next_undo_group_)};
}

WriterToken::WriterToken(WriterToken&& other) noexcept
    : id_(std::exchange(other.id_, {})), owner_nonce_(std::exchange(other.owner_nonce_, 0)),
      next_transaction_(other.next_transaction_.load(std::memory_order_relaxed)),
      next_command_(other.next_command_.load(std::memory_order_relaxed)),
      next_undo_group_(other.next_undo_group_.load(std::memory_order_relaxed)) {}

WriterToken& WriterToken::operator=(WriterToken&& other) noexcept {
    if (this != &other) {
        id_ = std::exchange(other.id_, {});
        owner_nonce_ = std::exchange(other.owner_nonce_, 0);
        next_transaction_.store(other.next_transaction_.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        next_command_.store(other.next_command_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        next_undo_group_.store(other.next_undo_group_.load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
    }
    return *this;
}

runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
DocumentSession::create(Project initial, SessionLimits limits) {
    return create(std::move(initial), limits, {});
}

runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
DocumentSession::create(Project initial, SessionLimits limits,
                        std::shared_ptr<JournalSink> journal_sink) {
    if (limits.max_writers == 0) {
        TransactionError value;
        value.code = ConflictCode::WriterLimit;
        return failure<std::unique_ptr<DocumentSession>>(value);
    }
    const auto nonce = allocate_sequence(g_next_session_nonce);
    if (nonce == 0) {
        TransactionError value;
        value.code = ConflictCode::SequenceExhausted;
        return failure<std::unique_ptr<DocumentSession>>(value);
    }
    if (journal_sink) {
        auto durable = journal_sink->checkpoint(initial, {});
        if (!durable) {
            TransactionError value;
            value.code = ConflictCode::JournalDurability;
            return failure<std::unique_ptr<DocumentSession>>(value);
        }
    }
    return runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>(
        runtime::Ok(std::unique_ptr<DocumentSession>(new DocumentSession(
            std::make_unique<Impl>(std::move(initial), limits, nonce, std::move(journal_sink))))));
}

std::uint64_t detail::SessionNonceTestAccess::exchange_next(std::uint64_t value) noexcept {
    return g_next_session_nonce.exchange(value, std::memory_order_relaxed);
}

DocumentSession::~DocumentSession() = default;
DocumentSession::DocumentSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

runtime::Result<WriterToken, TransactionError> DocumentSession::register_writer() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->writers.size() >= impl_->limits.max_writers || impl_->next_writer == 0 ||
        impl_->next_writer == std::numeric_limits<std::uint64_t>::max()) {
        TransactionError value;
        value.code = ConflictCode::WriterLimit;
        return failure<WriterToken>(value);
    }
    const WriterId id{impl_->next_writer++};
    impl_->writers.push_back({id, 0, 0});
    WriterToken token;
    token.id_ = id;
    token.owner_nonce_ = impl_->session_nonce;
    return runtime::Result<WriterToken, TransactionError>(runtime::Ok(std::move(token)));
}

DocumentView DocumentSession::current() const noexcept {
    const auto state = std::atomic_load_explicit(&impl_->published, std::memory_order_acquire);
    return {state->snapshot, state->revision};
}

std::shared_ptr<const Project> DocumentSession::snapshot() const noexcept {
    return current().snapshot;
}

DocumentRevision DocumentSession::revision() const noexcept {
    return current().revision;
}

runtime::Result<CommitResult, TransactionError> DocumentSession::submit(WriterToken& writer,
                                                                        Transaction transaction) {
    std::lock_guard lock(impl_->mutex);
    if (writer.owner_nonce_ != impl_->session_nonce || writer.id_ != transaction.id.writer) {
        return failure<CommitResult>(
            error(ConflictCode::InvalidIdentifier, transaction, revision()));
    }
    return impl_->commit_locked(std::move(transaction), true, true, CommitKind::Ordinary);
}

runtime::Result<CommitResult, TransactionError> DocumentSession::undo(WriterToken& writer) {
    std::lock_guard lock(impl_->mutex);
    if (writer.owner_nonce_ != impl_->session_nonce) {
        TransactionError value;
        value.code = ConflictCode::InvalidIdentifier;
        return failure<CommitResult>(value);
    }
    if (impl_->undo.empty()) {
        TransactionError value;
        value.code = ConflictCode::NothingToUndo;
        value.current_revision = revision();
        return failure<CommitResult>(value);
    }
    if (impl_->open_gesture) {
        TransactionError value;
        value.code = ConflictCode::GestureState;
        value.current_revision = revision();
        return failure<CommitResult>(value);
    }
    const auto record = impl_->undo.back();
    auto transaction = impl_->make_history_transaction(writer, record.inverse);
    auto result = impl_->commit_locked(std::move(transaction), false, false, CommitKind::History);
    if (!result)
        return result;
    impl_->undo_bytes -= impl_->undo.back().retained_bytes;
    impl_->undo.pop_back();
    impl_->redo_bytes = saturated_add(impl_->redo_bytes, record.retained_bytes);
    impl_->redo.push_back(record);
    return result;
}

runtime::Result<CommitResult, TransactionError> DocumentSession::redo(WriterToken& writer) {
    std::lock_guard lock(impl_->mutex);
    if (writer.owner_nonce_ != impl_->session_nonce) {
        TransactionError value;
        value.code = ConflictCode::InvalidIdentifier;
        return failure<CommitResult>(value);
    }
    if (impl_->open_gesture) {
        TransactionError value;
        value.code = ConflictCode::GestureState;
        value.current_revision = revision();
        return failure<CommitResult>(value);
    }
    if (impl_->redo.empty()) {
        TransactionError value;
        value.code = ConflictCode::NothingToRedo;
        value.current_revision = revision();
        return failure<CommitResult>(value);
    }
    const auto record = impl_->redo.back();
    auto transaction = impl_->make_history_transaction(writer, record.forward);
    auto result = impl_->commit_locked(std::move(transaction), false, false, CommitKind::History);
    if (!result)
        return result;
    impl_->redo_bytes -= impl_->redo.back().retained_bytes;
    impl_->redo.pop_back();
    impl_->undo_bytes = saturated_add(impl_->undo_bytes, record.retained_bytes);
    impl_->undo.push_back(record);
    return result;
}

bool DocumentSession::can_undo() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->open_gesture && !impl_->undo.empty();
}

bool DocumentSession::can_redo() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->open_gesture && !impl_->redo.empty();
}

CommandJournal DocumentSession::journal() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->journal;
}

bool DocumentSession::checkpoint(DocumentRevision durable_revision) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->journal_sink_failed)
        return false;
    if (durable_revision == impl_->journal.base_revision())
        return true;
    if (!impl_->journal_sink)
        return detail::JournalAccess::checkpoint(impl_->journal, durable_revision);
    auto checkpointed = impl_->journal;
    if (!detail::JournalAccess::checkpoint(checkpointed, durable_revision))
        return false;
    const auto* snapshot = detail::JournalAccess::checkpoint_snapshot(checkpointed);
    if (!snapshot)
        return false;
    auto durable = impl_->journal_sink->checkpoint(*snapshot, durable_revision);
    if (!durable) {
        impl_->journal_sink_failed = true;
        return false;
    }
    impl_->journal = std::move(checkpointed);
    return true;
}

} // namespace pulp::timeline
