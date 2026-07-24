#pragma once

#include <pulp/timeline/journal.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace pulp::timeline {

enum class FileJournalErrorCode : std::uint8_t {
    IoError,
    InvalidFormat,
    UnsupportedVersion,
    CorruptRecord,
    RevisionMismatch,
    LimitExceeded,
    PersistenceError,
    AlreadyOpen,
    AliasedPath,
    DurabilityUncertain,
};

struct FileJournalError {
    FileJournalErrorCode code = FileJournalErrorCode::IoError;
    std::uint64_t byte_offset = 0;
    std::optional<PersistenceError> persistence_error;
};

struct FileJournalLimits {
    std::uint64_t max_file_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    std::size_t max_record_bytes = 1024ull * 1024ull * 1024ull;
    DecodeLimits decode;
};

class FileJournal;

struct FileJournalOpenResult {
    std::shared_ptr<FileJournal> sink;
    Project checkpoint;
    DocumentRevision revision;
    bool recovered_existing = false;
    bool repaired_torn_tail = false;
};

/// Native, crash-consistent Timeline persistence.
///
/// Each committed revision is a checksummed frame containing one canonical
/// snapshot. A trailing partial frame is discarded during recovery; corruption
/// before the trailing frame fails closed. Checkpoints replace the file through
/// a durable temporary sibling and atomic rename.
class FileJournal final : public JournalSink {
  public:
    static runtime::Result<FileJournalOpenResult, FileJournalError>
    open(const std::filesystem::path& path, Project fallback, SchemaRegistry registry,
         const FileJournalLimits& limits = {});

    ~FileJournal() override;

    FileJournal(const FileJournal&) = delete;
    FileJournal& operator=(const FileJournal&) = delete;

    runtime::Result<bool, JournalSinkError>
    append_batch(const JournalEntry& entry) noexcept override;
    runtime::Result<bool, JournalSinkError>
    checkpoint(const Project& snapshot, DocumentRevision durable_revision) noexcept override;
    runtime::Result<bool, JournalSinkError>
    validate_restore(const Project& snapshot, DocumentRevision durable_revision) noexcept override;

  private:
    struct Impl;
    explicit FileJournal(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::timeline
