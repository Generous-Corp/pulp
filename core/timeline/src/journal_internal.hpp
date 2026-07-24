#pragma once

#include <pulp/timeline/journal.hpp>

namespace pulp::timeline::detail {

class JournalAccess {
  public:
    static runtime::Result<bool, TransactionError> preflight(const CommandJournal& journal,
                                                             const JournalEntry& entry) {
        return journal.preflight(entry);
    }
    static std::optional<Project> prepare_append(CommandJournal& journal, const Project& before) {
        return journal.prepare_append(before);
    }
    static void append_prepared(CommandJournal& journal, JournalEntry entry,
                                std::optional<Project> initial_snapshot) noexcept {
        journal.append_prepared(std::move(entry), std::move(initial_snapshot));
    }
    static void append(CommandJournal& journal, JournalEntry entry, const Project& before) {
        auto initial_snapshot = prepare_append(journal, before);
        append_prepared(journal, std::move(entry), std::move(initial_snapshot));
    }
    static bool checkpoint(CommandJournal& journal, DocumentRevision revision) {
        return journal.checkpoint(revision);
    }
    static const Project* checkpoint_snapshot(const CommandJournal& journal) noexcept {
        return journal.base_snapshot_ ? &*journal.base_snapshot_ : nullptr;
    }
    static void restore_checkpoint(CommandJournal& journal, const Project& checkpoint,
                                   DocumentRevision revision) {
        journal.base_snapshot_ = checkpoint;
        journal.base_revision_ = revision;
    }
};

} // namespace pulp::timeline::detail
