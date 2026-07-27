#pragma once

#include <pulp/timeline/command.hpp>
#include <pulp/timeline/compile_context.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline {

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

constexpr DirtyFlags operator|(DirtyFlags lhs, DirtyFlags rhs) noexcept {
    return static_cast<DirtyFlags>(static_cast<std::uint16_t>(lhs) |
                                   static_cast<std::uint16_t>(rhs));
}

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

class DirtySet {
  public:
    explicit DirtySet(std::vector<DirtyItem> items = {},
                      std::vector<DirtyContext> contexts = {});
    std::span<const DirtyItem> items() const noexcept {
        return items_;
    }
    std::span<const DirtyContext> contexts() const noexcept {
        return contexts_;
    }
    std::size_t retained_size() const noexcept;

  private:
    std::vector<DirtyItem> items_;
    std::vector<DirtyContext> contexts_;
};

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

struct ReducedTransaction {
    Project project;
    DirtySet dirty;
    std::vector<Command> inverses;
};

struct CommitResult {
    std::shared_ptr<const Project> snapshot;
    DocumentRevision revision;
    DirtySet dirty;
    std::vector<CommandId> applied_commands;
};

runtime::Result<ReducedTransaction, TransactionError>
reduce_transaction(const Project& project, const Transaction& transaction);

} // namespace pulp::timeline
