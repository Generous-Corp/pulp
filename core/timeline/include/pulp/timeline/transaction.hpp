#pragma once

#include <pulp/timeline/command.hpp>

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

class DirtySet {
  public:
    explicit DirtySet(std::vector<DirtyItem> items = {});
    std::span<const DirtyItem> items() const noexcept {
        return items_;
    }
    std::size_t retained_size() const noexcept;

  private:
    std::vector<DirtyItem> items_;
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

// Replays commands already authorized as undo/redo history. This reducer can
// reactivate exact tombstones in caller-owned values; DocumentSession::submit
// always uses reduce_transaction and cannot be elevated through this API.
runtime::Result<ReducedTransaction, TransactionError>
reduce_history_transaction(const Project& project, const Transaction& transaction);

} // namespace pulp::timeline
