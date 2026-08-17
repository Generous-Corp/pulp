#include <pulp/timeline/document_session.hpp>

#include "document_session_internal.hpp"

#include "owned_identity_traversal.hpp"

#include "journal_internal.hpp"
#include "session_nonce_test_access.hpp"
#include "transaction_internal.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::timeline {
namespace {

std::atomic<std::uint64_t> g_next_session_nonce{1};

struct WriterState {
    WriterId id;
    std::uint64_t transaction_watermark = 0;
    std::uint64_t command_watermark = 0;
    WriterCapabilityMask capabilities{};
    std::size_t session_retained_bytes = 0;
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

static_assert(std::is_nothrow_move_constructible_v<CachedCommit>);
static_assert(std::is_nothrow_move_assignable_v<CachedCommit>);
static_assert(std::is_nothrow_move_assignable_v<UndoRecord>);
static_assert(std::is_nothrow_move_constructible_v<CommitResult>);

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

// Admission runs before any reduction, so a refusal here cannot have applied a
// prefix of the batch.
std::optional<TransactionError> refuse_by_class(const WriterState& writer,
                                                const Transaction& transaction,
                                                const Project& project,
                                                DocumentRevision current);

std::optional<CommandClass> identity_command_class(ItemKind kind) noexcept {
    switch (kind) {
    case ItemKind::Sequence:
        return CommandClass::Sequence;
    case ItemKind::Track:
        return CommandClass::Track;
    case ItemKind::Clip:
        return CommandClass::Clip;
    case ItemKind::Note:
    case ItemKind::MidiLane:
    case ItemKind::MidiLanePoint:
        return CommandClass::Note;
    case ItemKind::DevicePlacement:
        return CommandClass::Device;
    case ItemKind::AutomationLane:
    case ItemKind::AutomationPoint:
    case ItemKind::Modulator:
    case ItemKind::MacroControl:
    case ItemKind::ModulationRoute:
        return CommandClass::Automation;
    case ItemKind::TakeLane:
    case ItemKind::Take:
        return CommandClass::Take;
    case ItemKind::Marker:
    case ItemKind::Region:
        return CommandClass::Annotation;
    case ItemKind::Scene:
    case ItemKind::Slot:
        return CommandClass::Scene;
    case ItemKind::Project:
    case ItemKind::Asset:
        return std::nullopt;
    }
    return std::nullopt;
}

template <typename Visit>
void add_owned_authorities(std::uint64_t& required, CommandIntent intent, Visit&& visit) {
    visit([&](const detail::ModelOwnedIdentity& identity) {
        if (const auto command_class = identity_command_class(identity.kind))
            required |= capability_bit({*command_class, intent});
    });
}

void add_clip_authorities(std::uint64_t& required, const Clip& clip, CommandIntent intent) {
    add_owned_authorities(required, intent, [&](auto&& visitor) {
        detail::visit_clip_owned_identities(clip, {}, std::forward<decltype(visitor)>(visitor));
    });
}

void add_track_authorities(std::uint64_t& required, const Track& track, CommandIntent intent) {
    add_owned_authorities(required, intent, [&](auto&& visitor) {
        detail::visit_track_owned_identities(track, std::forward<decltype(visitor)>(visitor));
    });
}

void add_sequence_authorities(std::uint64_t& required, const Sequence& sequence,
                              CommandIntent intent) {
    add_owned_authorities(required, intent, [&](auto&& visitor) {
        detail::visit_sequence_owned_identities(sequence,
                                                std::forward<decltype(visitor)>(visitor));
    });
}

void add_possible_track_child_authorities(std::uint64_t& required, CommandIntent intent) {
    for (const auto command_class : {CommandClass::Clip, CommandClass::Note,
                                     CommandClass::Automation, CommandClass::Take,
                                     CommandClass::Device})
        required |= capability_bit({command_class, intent});
}

void add_possible_sequence_child_authorities(std::uint64_t& required, CommandIntent intent) {
    add_possible_track_child_authorities(required, intent);
    for (const auto command_class :
         {CommandClass::Track, CommandClass::Annotation, CommandClass::Scene})
        required |= capability_bit({command_class, intent});
}

std::vector<std::uint64_t> note_ids(const std::vector<NoteEvent>& notes) {
    std::vector<std::uint64_t> result;
    result.reserve(notes.size());
    for (const auto& note : notes)
        result.push_back(note.id.value);
    std::sort(result.begin(), result.end());
    return result;
}

std::uint64_t required_authorities(const Command& command, const Project& project,
                                   bool has_prior_commands) {
    auto required = capability_bit(command_authority(command));
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InsertClip>) {
                add_clip_authorities(required, value.clip, CommandIntent::Create);
            } else if constexpr (std::is_same_v<T, RemoveClip>) {
                bool found = false;
                if (const auto* sequence = project.find_sequence(value.sequence_id))
                    if (const auto* track = sequence->find_track(value.track_id))
                        if (const auto* clip = track->find_clip(value.clip_id)) {
                            add_clip_authorities(required, *clip, CommandIntent::Remove);
                            found = true;
                        }
                // A prior command in the same atomic batch may create the
                // target. Without a current subtree to inspect, fail closed on
                // the only distinct child class a clip can own.
                if (!found || has_prior_commands)
                    required |= capability_bit({CommandClass::Note, CommandIntent::Remove});
            } else if constexpr (std::is_same_v<T, ReplaceNoteContent>) {
                const auto expected = note_ids(value.expected);
                const auto replacement = note_ids(value.replacement);
                if (!std::includes(expected.begin(), expected.end(), replacement.begin(),
                                   replacement.end()))
                    required |= capability_bit({CommandClass::Note, CommandIntent::Create});
                if (!std::includes(replacement.begin(), replacement.end(), expected.begin(),
                                   expected.end()))
                    required |= capability_bit({CommandClass::Note, CommandIntent::Remove});
            } else if constexpr (std::is_same_v<T, InsertTrack>) {
                add_track_authorities(required, value.track, CommandIntent::Create);
            } else if constexpr (std::is_same_v<T, RemoveTrack>) {
                bool found = false;
                if (const auto* sequence = project.find_sequence(value.sequence_id))
                    if (const auto* track = sequence->find_track(value.track_id)) {
                        add_track_authorities(required, *track, CommandIntent::Remove);
                        found = true;
                    }
                if (!found || has_prior_commands)
                    add_possible_track_child_authorities(required, CommandIntent::Remove);
            } else if constexpr (std::is_same_v<T, InsertSequence>) {
                add_sequence_authorities(required, value.sequence, CommandIntent::Create);
            } else if constexpr (std::is_same_v<T, CloneSequence>) {
                if (const auto* sequence = project.find_sequence(value.source_sequence_id)) {
                    add_sequence_authorities(required, *sequence, CommandIntent::Create);
                } else {
                    add_possible_sequence_child_authorities(required, CommandIntent::Create);
                }
            } else if constexpr (std::is_same_v<T, RemoveSequence>) {
                if (const auto* sequence = project.find_sequence(value.sequence_id)) {
                    add_sequence_authorities(required, *sequence, CommandIntent::Remove);
                } else {
                    add_possible_sequence_child_authorities(required, CommandIntent::Remove);
                }
                if (has_prior_commands)
                    add_possible_sequence_child_authorities(required, CommandIntent::Remove);
            }
        },
        command);
    return required;
}

std::optional<TransactionError> refuse_by_class(const WriterState& writer,
                                                const Transaction& transaction,
                                                const Project& project,
                                                DocumentRevision current) {
    bool has_prior_commands = false;
    for (const auto& envelope : transaction.commands) {
        if ((required_authorities(envelope.command, project, has_prior_commands) &
             ~writer.capabilities.allowed) != 0)
            return error(ConflictCode::CapabilityDenied, transaction, current, envelope.id);
        has_prior_commands = true;
    }
    return std::nullopt;
}

// A quota bounds what a writer adds to the document, so it is charged when that
// writer submits and not when history replays content already accounted for.
std::optional<TransactionError> refuse_by_quota(const WriterState& writer,
                                                const Transaction& transaction,
                                                DocumentRevision current) {
    const auto size = retained_size(transaction);
    if (size > writer.capabilities.max_transaction_retained_bytes ||
        saturated_add(writer.session_retained_bytes, size) >
            writer.capabilities.max_session_retained_bytes)
        return error(ConflictCode::WriterQuotaExhausted, transaction, current);
    return std::nullopt;
}

} // namespace

struct DocumentSession::Impl {
    explicit Impl(Project project, DocumentRevision revision, SessionLimits session_limits,
                  std::uint64_t nonce, std::shared_ptr<JournalSink> sink)
        : published(std::make_shared<const PublishedState>(
              PublishedState{std::make_shared<const Project>(std::move(project)), revision})),
          journal(session_limits.journal), limits(session_limits), session_nonce(nonce),
          journal_sink(std::move(sink)) {
        if (revision != DocumentRevision{})
            detail::JournalAccess::restore_checkpoint(journal, *published->snapshot, revision);
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

    const CachedCommit* find_cached(TransactionId id) const noexcept {
        const auto found = std::find_if(cache.begin(), cache.end(),
                                        [&](const CachedCommit& value) {
                                            return value.transaction.id == id;
                                        });
        return found == cache.end() ? nullptr : &*found;
    }

    runtime::Result<CommitResult, TransactionError>
    commit_locked(Transaction transaction, bool record_undo, bool clear_redo, CommitKind kind) {
        const auto current = std::atomic_load_explicit(&published, std::memory_order_relaxed);
        const auto current_revision = current->revision;
        auto* writer = find_writer(transaction.id.writer);
        if (!writer || !transaction.id.valid())
            return failure<CommitResult>(
                error(ConflictCode::InvalidIdentifier, transaction, current_revision));

        const auto* cached = find_cached(transaction.id);
        if (cached != nullptr) {
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
        case GesturePhase::Cancel:
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
        std::optional<UndoRecord> coalesced_undo;
        std::size_t undo_evictable_groups = 0;
        std::size_t undo_evictable_bytes = 0;
        bool undo_will_coalesce = false;
        if (record_undo) {
            candidate.writer = transaction.id.writer;
            candidate.group = transaction.undo_group;
            candidate.closed = transaction.gesture_phase == GesturePhase::Single ||
                               transaction.gesture_phase == GesturePhase::End ||
                               transaction.gesture_phase == GesturePhase::Cancel;
            for (const auto& envelope : transaction.commands)
                candidate.forward.push_back(envelope.command);
            candidate.inverse = reduced->inverses;
            candidate.retained_bytes =
                saturated_add(retained_size(candidate.forward), retained_size(candidate.inverse));

            undo_will_coalesce = candidate.group && !undo.empty() &&
                                 undo.back().group == candidate.group &&
                                 undo.back().writer == candidate.writer && !undo.back().closed;
            auto projected_groups = undo.size() + (undo_will_coalesce ? 0 : 1);
            auto projected_bytes = saturated_add(undo_bytes, candidate.retained_bytes);
            while ((projected_groups - undo_evictable_groups > limits.undo.max_groups ||
                    projected_bytes - std::min(projected_bytes, undo_evictable_bytes) >
                        limits.undo.max_retained_bytes) &&
                   undo_evictable_groups < undo.size() &&
                   undo[undo_evictable_groups].closed) {
                undo_evictable_bytes = saturated_add(
                    undo_evictable_bytes, undo[undo_evictable_groups].retained_bytes);
                ++undo_evictable_groups;
            }
            if (projected_groups - undo_evictable_groups > limits.undo.max_groups ||
                projected_bytes - std::min(projected_bytes, undo_evictable_bytes) >
                    limits.undo.max_retained_bytes)
                return failure<CommitResult>(
                    error(ConflictCode::UndoFull, transaction, current_revision));

            if (undo_will_coalesce) {
                coalesced_undo.emplace(undo.back());
                coalesced_undo->forward.insert(coalesced_undo->forward.end(),
                                               candidate.forward.begin(),
                                               candidate.forward.end());
                coalesced_undo->inverse.insert(coalesced_undo->inverse.begin(),
                                               candidate.inverse.begin(),
                                               candidate.inverse.end());
                coalesced_undo->retained_bytes =
                    saturated_add(coalesced_undo->retained_bytes, candidate.retained_bytes);
                coalesced_undo->closed = candidate.closed;
            }
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

        CommitResult result{published, next_revision, reduced->dirty, {}, current->snapshot};
        result.applied_commands.reserve(transaction.commands.size());
        for (const auto& envelope : transaction.commands)
            result.applied_commands.push_back(envelope.id);

        std::optional<CachedCommit> pending_cache;
        if (limits.max_cached_results > 0)
            pending_cache.emplace(CachedCommit{transaction, result});

        // A new deque node can allocate. Stage the node before asking the sink
        // to acknowledge durability, then fill it with a non-throwing move.
        const bool staged_undo_slot = record_undo && !undo_will_coalesce;
        if (staged_undo_slot)
            undo.emplace_back();
        const auto undo_added_bytes = candidate.retained_bytes;

        if (journal_sink) {
            auto durable = journal_sink->append_batch(journal_entry);
            if (!durable || !durable.value()) {
                if (staged_undo_slot)
                    undo.pop_back();
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

        if (record_undo) {
            for (std::size_t evicted = 0; evicted < undo_evictable_groups; ++evicted) {
                undo_bytes -= undo.front().retained_bytes;
                undo.pop_front();
            }
            if (undo_will_coalesce)
                undo.back() = std::move(*coalesced_undo);
            else
                undo.back() = std::move(candidate);
            undo_bytes = saturated_add(undo_bytes, undo_added_bytes);
            if (transaction.gesture_phase == GesturePhase::Begin)
                open_gesture = OpenGesture{transaction.id.writer, *transaction.undo_group};
            else if (transaction.gesture_phase == GesturePhase::End ||
                     transaction.gesture_phase == GesturePhase::Cancel)
                open_gesture.reset();
            if (clear_redo) {
                redo.clear();
                redo_bytes = 0;
            }
        }

        if (pending_cache) {
            if (cache.size() == limits.max_cached_results)
                cache.erase(cache.begin());
            cache.push_back(std::move(*pending_cache));
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
    return create_impl(std::move(initial), {}, limits, std::move(journal_sink),
                       SinkAttachment::Initialize);
}

runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
DocumentSession::restore(Project checkpoint, DocumentRevision checkpoint_revision,
                         SessionLimits limits, std::shared_ptr<JournalSink> journal_sink) {
    return create_impl(std::move(checkpoint), checkpoint_revision, limits,
                       std::move(journal_sink), SinkAttachment::Restore);
}

runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>
DocumentSession::create_impl(Project checkpoint, DocumentRevision checkpoint_revision,
                             SessionLimits limits, std::shared_ptr<JournalSink> journal_sink,
                             SinkAttachment attachment) {
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
        auto validated = attachment == SinkAttachment::Initialize
                             ? journal_sink->checkpoint(checkpoint, checkpoint_revision)
                             : journal_sink->validate_restore(checkpoint, checkpoint_revision);
        if (validated && validated.value() && attachment == SinkAttachment::Initialize)
            validated = journal_sink->validate_restore(checkpoint, checkpoint_revision);
        if (!validated || !validated.value()) {
            TransactionError value;
            value.code = ConflictCode::JournalDurability;
            return failure<std::unique_ptr<DocumentSession>>(value);
        }
    }
    return runtime::Result<std::unique_ptr<DocumentSession>, TransactionError>(
        runtime::Ok(std::unique_ptr<DocumentSession>(new DocumentSession(std::make_unique<Impl>(
            std::move(checkpoint), checkpoint_revision, limits, nonce, std::move(journal_sink))))));
}

std::uint64_t detail::SessionNonceTestAccess::exchange_next(std::uint64_t value) noexcept {
    return g_next_session_nonce.exchange(value, std::memory_order_relaxed);
}

DocumentSession::~DocumentSession() = default;
DocumentSession::DocumentSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CommandAuthority command_authority(const Command& command) noexcept {
    return std::visit(
        [](const auto& value) {
            return command_authority_of<std::decay_t<decltype(value)>>();
        },
        command);
}

runtime::Result<WriterToken, TransactionError> DocumentSession::register_writer() {
    return register_writer(unrestricted_capabilities());
}

runtime::Result<WriterToken, TransactionError>
DocumentSession::register_writer(WriterCapabilityMask mask) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->writers.size() >= impl_->limits.max_writers || impl_->next_writer == 0 ||
        impl_->next_writer == std::numeric_limits<std::uint64_t>::max()) {
        TransactionError value;
        value.code = ConflictCode::WriterLimit;
        return failure<WriterToken>(value);
    }
    const WriterId id{impl_->next_writer++};
    impl_->writers.push_back({id, 0, 0, mask, 0});
    WriterToken token;
    token.id_ = id;
    token.owner_nonce_ = impl_->session_nonce;
    return runtime::Result<WriterToken, TransactionError>(runtime::Ok(std::move(token)));
}

std::optional<WriterCapabilityMask>
DocumentSession::writer_capabilities(WriterToken::Provenance provenance) const noexcept {
    std::lock_guard lock(impl_->mutex);
    if (!provenance.valid() || provenance.owner_nonce_ != impl_->session_nonce)
        return std::nullopt;
    const auto* writer = impl_->find_writer(provenance.writer_);
    return writer ? std::optional<WriterCapabilityMask>(writer->capabilities) : std::nullopt;
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

bool DocumentSession::is_current_publication(const CommitResult& result) const noexcept {
    const auto state = std::atomic_load_explicit(&impl_->published, std::memory_order_acquire);
    return result.revision == state->revision && result.snapshot == state->snapshot;
}

bool DocumentSession::is_gesture_open(WriterToken::Provenance provenance,
                                      UndoGroupId group) const noexcept {
    std::lock_guard lock(impl_->mutex);
    return provenance.owner_nonce_ == impl_->session_nonce && provenance.writer_ == group.writer &&
           impl_->open_gesture && impl_->open_gesture->writer == provenance.writer_ &&
           impl_->open_gesture->group == group;
}

runtime::Result<CommitResult, TransactionError> DocumentSession::submit(WriterToken& writer,
                                                                        Transaction transaction) {
    std::lock_guard lock(impl_->mutex);
    if (writer.owner_nonce_ != impl_->session_nonce || writer.id_ != transaction.id.writer) {
        return failure<CommitResult>(
            error(ConflictCode::InvalidIdentifier, transaction, revision()));
    }
    auto* state = impl_->find_writer(writer.id_);
    std::size_t charge = 0;
    const auto* cached = impl_->find_cached(transaction.id);
    if (state != nullptr) {
        // Established identifier outcomes precede capability/quota admission:
        // none can mutate the document, and callers rely on their structured
        // collision/expired-retry errors to decide whether retry is meaningful.
        if (!transaction.id.valid() || cached != nullptr ||
            transaction.id.sequence <= state->transaction_watermark)
            return impl_->commit_locked(std::move(transaction), true, true,
                                        CommitKind::Ordinary);
        const auto view = current();
        if (auto refusal = refuse_by_class(*state, transaction, *view.snapshot, view.revision))
            return failure<CommitResult>(*refusal);
        if (auto refusal = refuse_by_quota(*state, transaction, view.revision))
            return failure<CommitResult>(*refusal);
        charge = retained_size(transaction);
    }
    auto result = impl_->commit_locked(std::move(transaction), true, true, CommitKind::Ordinary);
    if (result && state != nullptr)
        state->session_retained_bytes = saturated_add(state->session_retained_bytes, charge);
    return result;
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
    auto record = impl_->undo.back();
    auto transaction = impl_->make_history_transaction(writer, record.inverse);
    if (auto* state = impl_->find_writer(writer.id_)) {
        const auto view = current();
        if (auto refusal = refuse_by_class(*state, transaction, *view.snapshot, view.revision))
            return failure<CommitResult>(*refusal);
    }
    impl_->redo.emplace_back();
    auto result =
        impl_->commit_locked(std::move(transaction), false, false, CommitKind::History);
    if (!result)
        impl_->redo.pop_back();
    if (!result)
        return result;
    impl_->undo_bytes -= impl_->undo.back().retained_bytes;
    impl_->undo.pop_back();
    impl_->redo_bytes = saturated_add(impl_->redo_bytes, record.retained_bytes);
    impl_->redo.back() = std::move(record);
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
    auto record = impl_->redo.back();
    auto transaction = impl_->make_history_transaction(writer, record.forward);
    if (auto* state = impl_->find_writer(writer.id_)) {
        const auto view = current();
        if (auto refusal = refuse_by_class(*state, transaction, *view.snapshot, view.revision))
            return failure<CommitResult>(*refusal);
    }
    impl_->undo.emplace_back();
    auto result =
        impl_->commit_locked(std::move(transaction), false, false, CommitKind::History);
    if (!result)
        impl_->undo.pop_back();
    if (!result)
        return result;
    impl_->redo_bytes -= impl_->redo.back().retained_bytes;
    impl_->redo.pop_back();
    impl_->undo_bytes = saturated_add(impl_->undo_bytes, record.retained_bytes);
    impl_->undo.back() = std::move(record);
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
    if (!durable || !durable.value()) {
        impl_->journal_sink_failed = true;
        return false;
    }
    impl_->journal = std::move(checkpointed);
    return true;
}

runtime::Result<ReducedTransaction, TransactionError>
detail::DocumentSessionPreviewAccess::undo(const DocumentSession& session) {
    return preview(session, Direction::Undo);
}

runtime::Result<ReducedTransaction, TransactionError>
detail::DocumentSessionPreviewAccess::redo(const DocumentSession& session) {
    return preview(session, Direction::Redo);
}

runtime::Result<ReducedTransaction, TransactionError>
detail::DocumentSessionPreviewAccess::preview(const DocumentSession& session,
                                              Direction direction) {
    std::lock_guard lock(session.impl_->mutex);
    const auto current =
        std::atomic_load_explicit(&session.impl_->published, std::memory_order_relaxed);
    if (session.impl_->open_gesture) {
        TransactionError value;
        value.code = ConflictCode::GestureState;
        value.current_revision = current->revision;
        return failure<ReducedTransaction>(value);
    }
    const bool is_undo = direction == Direction::Undo;
    const auto& history = is_undo ? session.impl_->undo : session.impl_->redo;
    if (history.empty()) {
        TransactionError value;
        value.code = is_undo ? ConflictCode::NothingToUndo : ConflictCode::NothingToRedo;
        value.current_revision = current->revision;
        return failure<ReducedTransaction>(value);
    }
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.expected_revision = current->revision;
    const auto& commands = is_undo ? history.back().inverse : history.back().forward;
    transaction.commands.reserve(commands.size());
    std::uint64_t sequence = 1;
    for (const auto& command : commands)
        transaction.commands.push_back({{{1}, sequence++}, command});
    return detail::reduce_transaction(*current->snapshot, transaction, true);
}

} // namespace pulp::timeline
