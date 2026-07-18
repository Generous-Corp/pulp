#pragma once

#include <pulp/timeline/journal.hpp>

namespace pulp::timeline::detail {

class JournalAccess {
  public:
    static runtime::Result<bool, TransactionError> preflight(const CommandJournal& journal,
                                                             const JournalEntry& entry) {
        return journal.preflight(entry);
    }
    static void append(CommandJournal& journal, JournalEntry entry, const Project& before) {
        journal.append_preflighted(std::move(entry), before);
    }
    static bool checkpoint(CommandJournal& journal, DocumentRevision revision) {
        return journal.checkpoint(revision);
    }
};

} // namespace pulp::timeline::detail
