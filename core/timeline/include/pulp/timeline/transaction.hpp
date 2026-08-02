#pragma once

#include <pulp/timeline/command.hpp>
#include <pulp/timeline/compile_context.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_editing
 * @{
 */

/// Bitmask describing which interpretation of an item became stale.
enum class DirtyFlags : std::uint16_t {
    None = 0,
    Structure = 1 << 0,
    Timing = 1 << 1,
    Content = 1 << 2,
    Notes = 1 << 3,
    Added = 1 << 4,
    Removed = 1 << 5,
    Automation = 1 << 6,
    Take = 1 << 7,
    Freeze = 1 << 8,
    Marker = 1 << 9,
    // Sequence-owned context changed. The item names the sequence and owns no
    // track, so a consumer that maps dirty items to tracks finds nothing to
    // recompile here — which is correct, because the readers are not derivable
    // from the item. DirtySet::contexts() names the kind that changed; the
    // compiler resolves the readers through its subscription reverse index.
    Context = 1 << 10,
    Mixer = 1 << 11,
};

/// Combines independent dirty reasons.
constexpr DirtyFlags operator|(DirtyFlags lhs, DirtyFlags rhs) noexcept {
    return static_cast<DirtyFlags>(static_cast<std::uint16_t>(lhs) |
                                   static_cast<std::uint16_t>(rhs));
}

/// Canonical invalidation record for one item and its owning location.
///
/// owner_track is empty for sequence-owned items. Consumers use the owners to
/// route incremental recompilation without re-discovering the item hierarchy.
struct DirtyItem {
    ItemId item;
    ItemId owner_track;
    ItemId owner_sequence;
    DirtyFlags flags = DirtyFlags::None;
    constexpr auto operator<=>(const DirtyItem&) const = default;
};

/// One sequence-owned context kind a transaction changed.
///
/// Context is dirtied by kind rather than by item because its readers are not
/// its children: the edit is to a sequence lane, while the items that must
/// recompile are clips on tracks that declared they read that kind. Naming the
/// kind is what lets the compiler resolve exactly those readers instead of
/// widening to the whole sequence.
struct DirtyContext {
    ItemId owner_sequence;
    CompileContextKind kind = CompileContextKind::ChordScale;
    constexpr auto operator<=>(const DirtyContext&) const = default;
};

/// Owned canonical collection of incremental invalidations.
///
/// A DirtySet is immutable through its public API. Spans returned by accessors
/// remain valid until the set is destroyed or replaced.
class DirtySet {
  public:
    /// Takes ownership and canonicalizes dirty records.
    ///
    /// Items are sorted by sequence, track, and item; duplicate locations are
    /// merged by OR-ing flags. Contexts are sorted and deduplicated.
    explicit DirtySet(std::vector<DirtyItem> items = {},
                      std::vector<DirtyContext> contexts = {});
    /// Returns canonical item invalidations in stable sorted order.
    std::span<const DirtyItem> items() const noexcept {
        return items_;
    }
    /// Returns unique context invalidations in stable sorted order.
    std::span<const DirtyContext> contexts() const noexcept {
        return contexts_;
    }
    /// Returns the owned object and vector-storage size estimate.
    std::size_t retained_size() const noexcept;

  private:
    std::vector<DirtyItem> items_;
    std::vector<DirtyContext> contexts_;
};

/// Stable reason a transaction cannot be reduced or committed.
enum class ConflictCode : std::uint8_t {
    InvalidIdentifier,
    EmptyTransaction,
    StaleRevision,
    TransactionIdCollision,
    CommandIdCollision,
    AlreadyAppliedResultExpired,
    TargetMissing,
    WrongTargetKind,
    InactiveTarget,
    GestureState,
    ParentMismatch,
    ExpectedValueMismatch,
    IdentityNotAvailable,
    JournalFull,
    UndoFull,
    NothingToUndo,
    NothingToRedo,
    WriterLimit,
    SequenceExhausted,
    ModelInvariant,
    JournalDurability,
};

/// Structured transaction rejection.
///
/// transaction and command identify the rejected operation; item and
/// related_item identify model targets when applicable. expected_revision and
/// current_revision describe optimistic-concurrency failures. model_error
/// carries the underlying invariant rejection for ModelInvariant.
struct TransactionError {
    ConflictCode code = ConflictCode::ModelInvariant;
    TransactionId transaction;
    CommandId command;
    ItemId item;
    ItemId related_item;
    DocumentRevision expected_revision;
    DocumentRevision current_revision;
    std::optional<ModelError> model_error;
};

/// Pure reduction result before revision publication.
///
/// project is the candidate immutable value, dirty is its canonical
/// invalidation set, and inverses are ordered for undo application.
struct ReducedTransaction {
    Project project;
    DirtySet dirty;
    std::vector<Command> inverses;
};

/// Immutable state published by a successful session commit.
///
/// snapshot and revision are an atomic pair and remain valid independently of
/// later commits. predecessor_snapshot is the exact immutable snapshot reduced
/// by this commit, allowing downstream incremental consumers to prove lineage.
/// applied_commands lists IDs in transaction order.
struct CommitResult {
    std::shared_ptr<const Project> snapshot;
    DocumentRevision revision;
    DirtySet dirty;
    std::vector<CommandId> applied_commands;
    std::shared_ptr<const Project> predecessor_snapshot;
};

/// Purely applies a transaction to a project without session publication.
///
/// The input project is not mutated. The transaction must be nonempty and use
/// valid, unique command IDs belonging to its writer. Every command is reduced
/// in order; any conflict rejects the entire transaction and returns no partial
/// project. This public reducer does not restore tombstoned identities and is
/// not internally synchronized.
runtime::Result<ReducedTransaction, TransactionError>
reduce_transaction(const Project& project, const Transaction& transaction);

/// @}

} // namespace pulp::timeline
