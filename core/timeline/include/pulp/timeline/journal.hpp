#pragma once

#include <pulp/timeline/transaction.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline {

namespace detail {
class JournalAccess;
}

struct JournalLimits {
    std::size_t max_transactions = 1024;
    std::size_t max_commands = 8192;
    std::size_t max_retained_bytes = 16 * 1024 * 1024;
};

enum class JournalEntryKind : std::uint8_t { Ordinary, History };

struct JournalEntry {
    DocumentRevision before;
    DocumentRevision after;
    Transaction transaction;
    DirtySet dirty;
    JournalEntryKind kind = JournalEntryKind::Ordinary;
};

class CommandJournal {
  public:
    explicit CommandJournal(JournalLimits limits) : limits_(limits) {}

    runtime::Result<Project, TransactionError> replay(const Project& checkpoint,
                                                      DocumentRevision checkpoint_revision) const;

    std::span<const JournalEntry> entries() const noexcept {
        return entries_;
    }
    std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    std::size_t command_count() const noexcept {
        return command_count_;
    }
    DocumentRevision base_revision() const noexcept {
        return base_revision_;
    }

  private:
    struct WriterWatermark {
        WriterId id;
        std::uint64_t transaction = 0;
        std::uint64_t command = 0;
    };
    friend class detail::JournalAccess;
    runtime::Result<bool, TransactionError> preflight(const JournalEntry& entry) const;
    void append_preflighted(JournalEntry entry, const Project& before);
    bool checkpoint(DocumentRevision durable_revision);

    JournalLimits limits_;
    std::vector<JournalEntry> entries_;
    std::size_t retained_bytes_ = 0;
    std::size_t command_count_ = 0;
    DocumentRevision base_revision_{};
    std::optional<Project> base_snapshot_;
    std::vector<WriterWatermark> base_writers_;
};

std::size_t retained_size(const JournalEntry& entry) noexcept;

} // namespace pulp::timeline
