#pragma once

#include <pulp/timeline/command.hpp>
#include <pulp/timeline/journal.hpp>
#include <pulp/timeline/undo.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>

namespace pulp::timeline {

/** @addtogroup timeline_editing
 * @{
 */

namespace detail {
class DocumentSessionPreviewAccess;
class WriterTokenTestAccess;
} // namespace detail

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

/// The part of the document one command governs.
///
/// Classes partition `Command` exactly: every alternative maps to one class, so
/// an allowlist over classes is total rather than best-effort.
enum class CommandClass : std::uint8_t {
    /// Clip placement, timing, and playback properties on a track.
    Clip,
    /// Note content within a clip.
    Note,
    /// Automation lanes on a track.
    Automation,
    /// Track existence, order, naming, mix, arm, and freeze.
    Track,
    /// Take lanes, takes, and comping.
    Take,
    /// Launch scenes and their slots.
    Scene,
    /// Sequence existence and cloning.
    Sequence,
    /// Device placement, ordering, retargeting, and state.
    Device,
    /// Markers, regions, chord/scale lane, and groove.
    Annotation,
    /// Project tempo and meter maps.
    Timing,
    /// Project-owned media assets.
    Asset,
};

/// Number of distinct command classes.
inline constexpr std::size_t kCommandClassCount = 11;

/// What a command does to the entity its class names.
///
/// `Remove` is the destructive intent: it is the axis a capability mask denies
/// by default for an untrusted writer.
enum class CommandIntent : std::uint8_t {
    /// Brings a new entity into the document.
    Create,
    /// Changes an existing entity in place.
    Modify,
    /// Takes an existing entity out of the document.
    Remove,
};

/// Number of distinct command intents.
inline constexpr std::size_t kCommandIntentCount = 3;

/// The authority one command requires, as a class and intent pair.
struct CommandAuthority {
    /// The part of the document the command governs.
    CommandClass command_class = CommandClass::Clip;
    /// What the command does to that part.
    CommandIntent intent = CommandIntent::Modify;

    constexpr bool operator==(const CommandAuthority&) const noexcept = default;
};

namespace detail {
template <class> inline constexpr bool unclassified_command_v = false;
} // namespace detail

/// Returns the primary authority command type `T` requires.
///
/// Total over the `Command` variant by construction: an unclassified
/// alternative fails to compile rather than defaulting to a permissive answer,
/// so a command added to the variant cannot silently arrive unguarded. Stated
/// per type rather than per value so a conformance check can walk every
/// alternative without constructing one. Admission additionally accounts for
/// identity-bearing children carried by aggregate commands and for the note-ID
/// set difference in `ReplaceNoteContent`.
template <class T> constexpr CommandAuthority command_authority_of() noexcept {
    using Class = CommandClass;
    using Intent = CommandIntent;
    if constexpr (std::is_same_v<T, InsertClip>)
        return {Class::Clip, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveClip>)
        return {Class::Clip, Intent::Remove};
    else if constexpr (std::is_same_v<T, MoveClip>)
        return {Class::Clip, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetClipPlaybackProperties>)
        return {Class::Clip, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetClipSequenceRef>)
        return {Class::Clip, Intent::Modify};
    else if constexpr (std::is_same_v<T, InsertNotes>)
        return {Class::Note, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveNotes>)
        return {Class::Note, Intent::Remove};
    else if constexpr (std::is_same_v<T, SetNoteVelocity>)
        return {Class::Note, Intent::Modify};
    else if constexpr (std::is_same_v<T, ReplaceNoteContent>)
        return {Class::Note, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetNoteEvents>)
        return {Class::Note, Intent::Modify};
    else if constexpr (std::is_same_v<T, InsertAutomationLane>)
        return {Class::Automation, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveAutomationLane>)
        return {Class::Automation, Intent::Remove};
    else if constexpr (std::is_same_v<T, InsertTrack>)
        return {Class::Track, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveTrack>)
        return {Class::Track, Intent::Remove};
    else if constexpr (std::is_same_v<T, SetTrackName>)
        return {Class::Track, Intent::Modify};
    else if constexpr (std::is_same_v<T, MoveTrack>)
        return {Class::Track, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetTrackMixer>)
        return {Class::Track, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetRecordArm>)
        return {Class::Track, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetTrackFreeze>)
        return {Class::Track, Intent::Modify};
    else if constexpr (std::is_same_v<T, InsertTakeLane>)
        return {Class::Take, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveTakeLane>)
        return {Class::Take, Intent::Remove};
    else if constexpr (std::is_same_v<T, InsertTake>)
        return {Class::Take, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveTake>)
        return {Class::Take, Intent::Remove};
    else if constexpr (std::is_same_v<T, SetActiveTakeLane>)
        return {Class::Take, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetTakeComp>)
        return {Class::Take, Intent::Modify};
    else if constexpr (std::is_same_v<T, InsertScene>)
        return {Class::Scene, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveScene>)
        return {Class::Scene, Intent::Remove};
    else if constexpr (std::is_same_v<T, InsertSlot>)
        return {Class::Scene, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveSlot>)
        return {Class::Scene, Intent::Remove};
    else if constexpr (std::is_same_v<T, InsertSequence>)
        return {Class::Sequence, Intent::Create};
    else if constexpr (std::is_same_v<T, CloneSequence>)
        return {Class::Sequence, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveSequence>)
        return {Class::Sequence, Intent::Remove};
    else if constexpr (std::is_same_v<T, InsertDevice>)
        return {Class::Device, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveDevice>)
        return {Class::Device, Intent::Remove};
    else if constexpr (std::is_same_v<T, MoveDevice>)
        return {Class::Device, Intent::Modify};
    else if constexpr (std::is_same_v<T, RetargetDevice>)
        return {Class::Device, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetDeviceState>)
        return {Class::Device, Intent::Modify};
    else if constexpr (std::is_same_v<T, InsertMarker>)
        return {Class::Annotation, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveMarker>)
        return {Class::Annotation, Intent::Remove};
    else if constexpr (std::is_same_v<T, InsertRegion>)
        return {Class::Annotation, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveRegion>)
        return {Class::Annotation, Intent::Remove};
    else if constexpr (std::is_same_v<T, SetChordScaleLane>)
        return {Class::Annotation, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetGroove>)
        return {Class::Annotation, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetTempoMap>)
        return {Class::Timing, Intent::Modify};
    else if constexpr (std::is_same_v<T, SetMeterMap>)
        return {Class::Timing, Intent::Modify};
    else if constexpr (std::is_same_v<T, CreateAsset>)
        return {Class::Asset, Intent::Create};
    else if constexpr (std::is_same_v<T, RemoveAsset>)
        return {Class::Asset, Intent::Remove};
    else
        static_assert(detail::unclassified_command_v<T>,
                      "every Command alternative needs a CommandAuthority");
}

/// Returns the primary authority `command` requires.
CommandAuthority command_authority(const Command& command) noexcept;

/// Returns the single mask bit representing `authority`.
constexpr std::uint64_t capability_bit(CommandAuthority authority) noexcept {
    return std::uint64_t{1} << (static_cast<std::size_t>(authority.command_class) *
                                    kCommandIntentCount +
                                static_cast<std::size_t>(authority.intent));
}

/// Every class/intent pair set.
inline constexpr std::uint64_t kAllCommandAuthorities =
    (std::uint64_t{1} << (kCommandClassCount * kCommandIntentCount)) - 1;
static_assert(kCommandClassCount * kCommandIntentCount < 64,
              "writer capability pairs must fit in the mask");

/// Fixed authority of one writer over command classes and retained size.
///
/// A default-constructed mask denies every destructive intent and imposes no
/// quota. The no-argument registration overload explicitly uses
/// `unrestricted_capabilities()` so existing trusted callers retain their prior
/// authority. Quotas use the complete envelope-aware
/// `retained_size(const Transaction&)` estimate. A commandless End/Cancel may
/// close only the ephemeral gesture lifecycle without document publication,
/// journaling, capability admission, or quota charge.
struct WriterCapabilityMask {
    /// One bit per class/intent pair, indexed by `capability_bit`.
    std::uint64_t allowed = [] {
        auto value = kAllCommandAuthorities;
        for (std::size_t index = 0; index < kCommandClassCount; ++index)
            value &= ~capability_bit(
                {static_cast<CommandClass>(index), CommandIntent::Remove});
        return value;
    }();
    /// Largest retained size a single transaction from this writer may carry.
    std::size_t max_transaction_retained_bytes = std::numeric_limits<std::size_t>::max();
    /// Largest cumulative retained size this writer may commit to the session.
    std::size_t max_session_retained_bytes = std::numeric_limits<std::size_t>::max();
};

/// Reports whether `mask` permits `authority`.
constexpr bool allows(const WriterCapabilityMask& mask, CommandAuthority authority) noexcept {
    return (mask.allowed & capability_bit(authority)) != 0;
}

/// Returns `mask` with one class/intent pair denied.
constexpr WriterCapabilityMask deny(WriterCapabilityMask mask, CommandClass command_class,
                                    CommandIntent intent) noexcept {
    mask.allowed &= ~capability_bit({command_class, intent});
    return mask;
}

/// Returns `mask` with one class/intent pair permitted.
constexpr WriterCapabilityMask allow(WriterCapabilityMask mask, CommandClass command_class,
                                     CommandIntent intent) noexcept {
    mask.allowed |= capability_bit({command_class, intent});
    return mask;
}

/// Returns the legacy authority carried by writers registered without a mask.
constexpr WriterCapabilityMask unrestricted_capabilities() noexcept {
    WriterCapabilityMask mask;
    mask.allowed = kAllCommandAuthorities;
    return mask;
}

/// Returns a mask permitting every class but denying every destructive intent.
constexpr WriterCapabilityMask non_destructive_capabilities() noexcept {
    return WriterCapabilityMask{};
}

/// Move-only authority for submitting transactions to one DocumentSession.
///
/// A token is bound to the session that created it and remains valid until that
/// session is destroyed. Its three allocation methods are safe to call
/// concurrently and produce monotonically increasing, writer-scoped IDs.
/// Exhaustion produces an invalid ID rather than wrapping.
class WriterToken {
  public:
    /// Opaque identity of one writer authority within one document session.
    ///
    /// This copyable value supports equality checks only. It does not expose the
    /// session nonce and does not authorize submission or ID allocation.
    class Provenance {
      public:
        /// Constructs an invalid provenance value.
        constexpr Provenance() noexcept = default;

        /// Returns whether this value identifies a writer authority.
        constexpr bool valid() const noexcept {
            return writer_.valid() && owner_nonce_ != 0;
        }

        /// Compares both the session and writer identity without exposing either.
        constexpr bool operator==(const Provenance&) const noexcept = default;

      private:
        friend class WriterToken;
        friend class DocumentSession;
        constexpr Provenance(WriterId writer, std::uint64_t owner_nonce) noexcept
            : writer_(writer), owner_nonce_(owner_nonce) {}

        WriterId writer_;
        std::uint64_t owner_nonce_ = 0;
    };

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
    /// Returns an opaque fingerprint that remains stable when this token moves.
    [[nodiscard]] Provenance provenance() const noexcept {
        return Provenance(id_, owner_nonce_);
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
    ///
    /// The writer carries full authority and no quota.
    runtime::Result<WriterToken, TransactionError> register_writer();
    /// Registers a distinct writer whose authority is fixed at `mask`.
    ///
    /// The mask is copied into session state and is never reachable from the
    /// returned token, so it cannot be widened, narrowed, or replaced for the
    /// life of the writer. Submissions are admitted against it inside the
    /// session's own admission path, so there is no wrapper to bypass.
    ///
    /// Undo and redo are admitted against the same classes, because the inverse
    /// of a creation is a removal: without that check a writer denied a class
    /// could still reach its effect by undoing another writer's work. Quotas
    /// are not charged there, since replaying history adds no new content.
    runtime::Result<WriterToken, TransactionError> register_writer(WriterCapabilityMask mask);
    /// Returns the authority fixed at registration for `provenance`.
    ///
    /// Returns nullopt when the provenance does not name a writer of this
    /// session. This is an observation for tests and diagnostics; it hands back
    /// a copy and never a handle that could mutate the stored mask.
    std::optional<WriterCapabilityMask>
    writer_capabilities(WriterToken::Provenance provenance) const noexcept;
    /// Returns one atomic snapshot/revision observation without taking the writer lock.
    DocumentView current() const noexcept;
    /// Returns the currently published immutable snapshot.
    std::shared_ptr<const Project> snapshot() const noexcept;
    /// Returns the revision paired with the currently published snapshot.
    DocumentRevision revision() const noexcept;
    /// Reports whether result is the session's exact current publication.
    ///
    /// Snapshot identity and revision are compared from one atomic observation.
    bool is_current_publication(const CommitResult& result) const noexcept;
    /// Reports whether provenance/group owns the session's current open gesture.
    ///
    /// This control-thread query reads the authoritative session lifecycle under
    /// the writer lock. It retains neither the provenance nor submission authority.
    bool is_gesture_open(WriterToken::Provenance provenance, UndoGroupId group) const noexcept;
    /// Applies and, when configured, durably journals one transaction.
    ///
    /// The token must belong to this session and match the transaction writer.
    /// The transaction must target the current revision and use strictly
    /// increasing writer-scoped IDs. Equivalent retries within the result cache
    /// return their original CommitResult without applying twice. Successful
    /// document commits advance the revision by one and invalidate the redo
    /// stack. A commandless End/Cancel closes the matching open gesture without
    /// changing the document revision or journal; other empty transactions are
    /// rejected.
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
