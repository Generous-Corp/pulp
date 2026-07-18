#include <pulp/timeline/journal.hpp>

#include "transaction_internal.hpp"

#include <algorithm>
#include <limits>

namespace pulp::timeline {
namespace {

std::size_t saturated_add(std::size_t lhs, std::size_t rhs) noexcept {
    return rhs > std::numeric_limits<std::size_t>::max() - lhs
               ? std::numeric_limits<std::size_t>::max()
               : lhs + rhs;
}

TransactionError journal_error(const JournalEntry& entry) {
    TransactionError error;
    error.code = ConflictCode::JournalFull;
    error.transaction = entry.transaction.id;
    error.expected_revision = entry.before;
    error.current_revision = entry.before;
    return error;
}

bool same_dirty(const DirtySet& lhs, const DirtySet& rhs) noexcept {
    const auto left = lhs.items();
    const auto right = rhs.items();
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

std::size_t retained_size(const JournalEntry& entry) noexcept {
    return saturated_add(sizeof(JournalEntry), saturated_add(retained_size(entry.transaction),
                                                             entry.dirty.retained_size()));
}

runtime::Result<bool, TransactionError> CommandJournal::preflight(const JournalEntry& entry) const {
    const auto bytes = retained_size(entry);
    const auto commands = entry.transaction.commands.size();
    if (entries_.size() >= limits_.max_transactions ||
        commands > limits_.max_commands - std::min(command_count_, limits_.max_commands) ||
        bytes > limits_.max_retained_bytes - std::min(retained_bytes_, limits_.max_retained_bytes))
        return runtime::Result<bool, TransactionError>(runtime::Err(journal_error(entry)));
    return runtime::Result<bool, TransactionError>(runtime::Ok(true));
}

void CommandJournal::append_preflighted(JournalEntry entry) {
    retained_bytes_ += retained_size(entry);
    command_count_ += entry.transaction.commands.size();
    entries_.push_back(std::move(entry));
}

runtime::Result<Project, TransactionError>
CommandJournal::replay(const Project& checkpoint, DocumentRevision checkpoint_revision) const {
    Project current = checkpoint;
    auto revision = checkpoint_revision;
    for (const auto& entry : entries_) {
        if (entry.after.value <= checkpoint_revision.value)
            continue;
        if (entry.before != revision || entry.after.value != revision.value + 1) {
            TransactionError error;
            error.code = ConflictCode::StaleRevision;
            error.transaction = entry.transaction.id;
            error.expected_revision = entry.before;
            error.current_revision = revision;
            return runtime::Result<Project, TransactionError>(runtime::Err(error));
        }
        auto reduced = detail::reduce_transaction(current, entry.transaction,
                                                  entry.kind == JournalEntryKind::History);
        if (!reduced)
            return runtime::Result<Project, TransactionError>(runtime::Err(reduced.error()));
        if (!same_dirty(reduced->dirty, entry.dirty)) {
            auto error = TransactionError{};
            error.code = ConflictCode::ModelInvariant;
            error.transaction = entry.transaction.id;
            return runtime::Result<Project, TransactionError>(runtime::Err(error));
        }
        current = std::move(reduced).value().project;
        revision = entry.after;
    }
    return runtime::Result<Project, TransactionError>(runtime::Ok(std::move(current)));
}

bool CommandJournal::checkpoint(DocumentRevision durable_revision) {
    if (durable_revision.value < base_revision_.value)
        return false;
    const auto latest = entries_.empty() ? base_revision_ : entries_.back().after;
    if (durable_revision.value > latest.value)
        return false;
    const auto end = std::upper_bound(entries_.begin(), entries_.end(), durable_revision,
                                      [](DocumentRevision revision, const JournalEntry& entry) {
                                          return revision.value < entry.after.value;
                                      });
    entries_.erase(entries_.begin(), end);
    base_revision_ = durable_revision;
    retained_bytes_ = 0;
    command_count_ = 0;
    for (const auto& entry : entries_) {
        retained_bytes_ += retained_size(entry);
        command_count_ += entry.transaction.commands.size();
    }
    return true;
}

} // namespace pulp::timeline
