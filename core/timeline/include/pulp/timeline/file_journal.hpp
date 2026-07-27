#pragma once

#include <pulp/timeline/journal.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace pulp::timeline {

/** @addtogroup timeline_persistence
 * @{
 */

/// File-journal open or recovery failure category.
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

/// File-journal failure with the offending byte and nested JSON error.
///
/// persistence_error is populated when canonical snapshot decoding or encoding
/// fails; byte_offset identifies the record or frame position when applicable.
struct FileJournalError {
    FileJournalErrorCode code = FileJournalErrorCode::IoError;
    std::uint64_t byte_offset = 0;
    std::optional<PersistenceError> persistence_error;
};

/// File size, record size, and nested decode ceilings.
struct FileJournalLimits {
    std::uint64_t max_file_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    std::size_t max_record_bytes = 1024ull * 1024ull * 1024ull;
    DecodeLimits decode;
};

class FileJournal;

/// Result of opening or recovering a native Timeline journal.
///
/// sink retains the open file and must stay alive while a DocumentSession uses
/// it. checkpoint and revision are the exact state validated against the durable
/// file. The two flags distinguish a new fallback from recovered or repaired
/// durable state.
struct FileJournalOpenResult {
    std::shared_ptr<FileJournal> sink;
    Project checkpoint;
    DocumentRevision revision;
    bool recovered_existing = false;
    bool repaired_torn_tail = false;
};

/// Native, crash-consistent Timeline persistence sink.
///
/// Each committed revision is a checksummed frame containing one canonical
/// snapshot. A trailing partial frame is discarded during recovery; corruption
/// before the trailing frame fails closed. Checkpoints replace the file through
/// a durable temporary sibling and atomic rename. Public operations are
/// synchronous and perform filesystem I/O, so they must not run on an audio
/// thread. A path can have at most one live FileJournal in the process,
/// including aliases that resolve to the same file.
class FileJournal final : public JournalSink {
  public:
    /// Opens, creates, or recovers a journal at path.
    ///
    /// A new or empty journal is initialized from fallback at revision zero.
    /// Existing complete frames are validated and the newest snapshot is
    /// returned. A torn trailing frame is truncated only after the valid prefix
    /// is recovered; non-tail corruption fails closed. registry is retained by
    /// value for subsequent canonical serialization.
    ///
    /// @return An open sink and recovered checkpoint, or a bounded,
    ///         path-alias, format, revision, durability, or I/O failure.
    static runtime::Result<FileJournalOpenResult, FileJournalError>
    open(const std::filesystem::path& path, Project fallback, SchemaRegistry registry,
         const FileJournalLimits& limits = {});

    /// Flushes no additional state and releases the file and lifetime locks.
    ~FileJournal() override;

    FileJournal(const FileJournal&) = delete;
    FileJournal& operator=(const FileJournal&) = delete;

    /// Durably appends the entry's resulting canonical snapshot frame.
    runtime::Result<bool, JournalSinkError>
    append_batch(const JournalEntry& entry) noexcept override;
    /// Atomically replaces durable history with snapshot at durable_revision.
    runtime::Result<bool, JournalSinkError>
    checkpoint(const Project& snapshot, DocumentRevision durable_revision) noexcept override;
    /// Compares snapshot and revision with the sink's recovered durable state.
    ///
    /// This operation does not truncate or rewrite the file.
    runtime::Result<bool, JournalSinkError>
    validate_restore(const Project& snapshot, DocumentRevision durable_revision) noexcept override;

  private:
    struct Impl;
    explicit FileJournal(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// @}

} // namespace pulp::timeline
