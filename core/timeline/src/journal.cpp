#include <pulp/timeline/journal.hpp>

#include "snapshot_equivalence.hpp"
#include "transaction_internal.hpp"

#include <algorithm>
#include <limits>
#include <vector>

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

struct ReplayWriter {
    WriterId id;
    std::uint64_t transaction = 0;
    std::uint64_t command = 0;
};

} // namespace

std::size_t retained_size(const JournalEntry& entry) noexcept {
    return saturated_add(sizeof(JournalEntry), saturated_add(retained_size(entry.transaction),
                                                             entry.dirty.retained_size()));
}

runtime::Result<bool, TransactionError> CommandJournal::preflight(const JournalEntry& entry) const {
    const auto latest = entries_.empty() ? base_revision_ : entries_.back().after;
    if (entry.before != latest || entry.before.value == std::numeric_limits<std::uint64_t>::max() ||
        entry.after.value != entry.before.value + 1 ||
        entry.transaction.expected_revision != entry.before) {
        auto value = journal_error(entry);
        value.code = ConflictCode::StaleRevision;
        value.current_revision = latest;
        return runtime::Result<bool, TransactionError>(runtime::Err(value));
    }
    const auto bytes = retained_size(entry);
    const auto commands = entry.transaction.commands.size();
    if (entries_.size() >= limits_.max_transactions ||
        commands > limits_.max_commands - std::min(command_count_, limits_.max_commands) ||
        bytes > limits_.max_retained_bytes - std::min(retained_bytes_, limits_.max_retained_bytes))
        return runtime::Result<bool, TransactionError>(runtime::Err(journal_error(entry)));
    return runtime::Result<bool, TransactionError>(runtime::Ok(true));
}

std::optional<Project> CommandJournal::prepare_append(const Project& before) {
    entries_.reserve(entries_.size() + 1);
    return base_snapshot_ ? std::nullopt : std::optional<Project>(before);
}

void CommandJournal::append_prepared(JournalEntry entry,
                                     std::optional<Project> initial_snapshot) noexcept {
    if (initial_snapshot)
        base_snapshot_ = std::move(initial_snapshot);
    retained_bytes_ += retained_size(entry);
    command_count_ += entry.transaction.commands.size();
    entries_.push_back(std::move(entry));
}

runtime::Result<Project, TransactionError>
CommandJournal::replay(const Project& checkpoint, DocumentRevision checkpoint_revision) const {
    if (checkpoint_revision != base_revision_ ||
        (base_snapshot_ && !detail::snapshots_equivalent(checkpoint, *base_snapshot_))) {
        TransactionError error;
        error.code = checkpoint_revision != base_revision_ ? ConflictCode::StaleRevision
                                                           : ConflictCode::ModelInvariant;
        error.expected_revision = base_revision_;
        error.current_revision = checkpoint_revision;
        return runtime::Result<Project, TransactionError>(runtime::Err(error));
    }
    Project current = checkpoint;
    auto revision = checkpoint_revision;
    std::vector<ReplayWriter> writers;
    writers.reserve(base_writers_.size());
    for (const auto& writer : base_writers_)
        writers.push_back({writer.id, writer.transaction, writer.command});
    for (const auto& entry : entries_) {
        if (entry.before != revision || entry.after.value != revision.value + 1 ||
            entry.transaction.expected_revision != entry.before) {
            TransactionError error;
            error.code = ConflictCode::StaleRevision;
            error.transaction = entry.transaction.id;
            error.expected_revision = entry.before;
            error.current_revision = revision;
            return runtime::Result<Project, TransactionError>(runtime::Err(error));
        }
        auto writer = std::find_if(writers.begin(), writers.end(), [&](const ReplayWriter& value) {
            return value.id == entry.transaction.id.writer;
        });
        if (writer == writers.end()) {
            writers.push_back({entry.transaction.id.writer});
            writer = std::prev(writers.end());
        }
        if (entry.transaction.id.sequence <= writer->transaction) {
            auto error = journal_error(entry);
            error.code = ConflictCode::TransactionIdCollision;
            return runtime::Result<Project, TransactionError>(runtime::Err(error));
        }
        auto command_watermark = writer->command;
        for (const auto& envelope : entry.transaction.commands) {
            if (envelope.id.writer != entry.transaction.id.writer ||
                envelope.id.sequence <= command_watermark) {
                auto error = journal_error(entry);
                error.code = ConflictCode::CommandIdCollision;
                error.command = envelope.id;
                return runtime::Result<Project, TransactionError>(runtime::Err(error));
            }
            command_watermark = envelope.id.sequence;
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
        writer->transaction = entry.transaction.id.sequence;
        writer->command = command_watermark;
    }
    return runtime::Result<Project, TransactionError>(runtime::Ok(std::move(current)));
}

bool CommandJournal::checkpoint(DocumentRevision durable_revision) {
    if (durable_revision.value < base_revision_.value)
        return false;
    const auto latest = entries_.empty() ? base_revision_ : entries_.back().after;
    if (durable_revision.value > latest.value)
        return false;
    if (durable_revision == base_revision_)
        return true;
    if (!base_snapshot_)
        return false;
    const auto end = std::upper_bound(entries_.begin(), entries_.end(), durable_revision,
                                      [](DocumentRevision revision, const JournalEntry& entry) {
                                          return revision.value < entry.after.value;
                                      });
    CommandJournal prefix = *this;
    const auto prefix_count = static_cast<std::size_t>(std::distance(entries_.begin(), end));
    prefix.entries_.erase(std::next(prefix.entries_.begin(), prefix_count), prefix.entries_.end());
    auto reconstructed = prefix.replay(*base_snapshot_, base_revision_);
    if (!reconstructed)
        return false;

    auto next_writers = base_writers_;
    for (auto entry = entries_.begin(); entry != end; ++entry) {
        auto writer = std::find_if(
            next_writers.begin(), next_writers.end(),
            [&](const WriterWatermark& value) { return value.id == entry->transaction.id.writer; });
        if (writer == next_writers.end()) {
            next_writers.push_back({entry->transaction.id.writer});
            writer = std::prev(next_writers.end());
        }
        writer->transaction = std::max(writer->transaction, entry->transaction.id.sequence);
        for (const auto& command : entry->transaction.commands)
            writer->command = std::max(writer->command, command.id.sequence);
    }
    entries_.erase(entries_.begin(), end);
    base_revision_ = durable_revision;
    base_snapshot_ = std::move(reconstructed).value();
    base_writers_ = std::move(next_writers);
    retained_bytes_ = 0;
    command_count_ = 0;
    for (const auto& entry : entries_) {
        retained_bytes_ += retained_size(entry);
        command_count_ += entry.transaction.commands.size();
    }
    return true;
}

} // namespace pulp::timeline
