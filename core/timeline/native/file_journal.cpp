#include <pulp/timeline/file_journal.hpp>

#include "../internal/transaction_reduction.hpp"
#include "file_journal_codec.hpp"
#include "file_journal_native_io.hpp"

#include <pulp/runtime/detail/durable_file_replacement.hpp>
#include <pulp/timeline/serialize.hpp>

#include <limits>
#include <memory>
#include <utility>

namespace pulp::timeline {
namespace {

using detail::NativeFile;

runtime::Result<NativeFile, FileJournalError>
write_checkpoint_file(const std::filesystem::path& destination, const Project& snapshot,
                      DocumentRevision revision, const SchemaRegistry& registry,
                      const FileJournalLimits& limits) {
    auto encoded = detail::encode_snapshot(snapshot, registry, limits, detail::kFileHeaderBytes);
    if (!encoded)
        return runtime::Result<NativeFile, FileJournalError>(
            runtime::Err(std::move(encoded).error()));
    const auto fixed_bytes = detail::kFileHeaderBytes + detail::kFrameHeaderBytes;
    if (limits.max_file_bytes < fixed_bytes ||
        encoded.value().size() > limits.max_file_bytes - fixed_bytes)
        return detail::file_failure<NativeFile>(FileJournalErrorCode::LimitExceeded);

    auto replacement = runtime::detail::DurableFileReplacement::create(destination);
    if (!replacement)
        return detail::file_failure<NativeFile>(FileJournalErrorCode::IoError);
    const auto header = detail::file_header();
    if (!replacement->write_all(header) ||
        !detail::write_frame(*replacement, detail::kCheckpointFrame, revision, encoded.value()))
        return detail::file_failure<NativeFile>(FileJournalErrorCode::IoError);
    const auto committed = replacement->commit();
    if (committed == runtime::detail::DurableFileCommitOutcome::NotReplaced)
        return detail::file_failure<NativeFile>(FileJournalErrorCode::IoError);
    if (committed == runtime::detail::DurableFileCommitOutcome::ReplacedButDirectorySyncFailed)
        return detail::file_failure<NativeFile>(FileJournalErrorCode::DurabilityUncertain);

    auto reopened = NativeFile::open_existing(destination);
    if (!reopened.valid())
        return detail::file_failure<NativeFile>(FileJournalErrorCode::IoError);
    const auto links = reopened.link_count();
    if (!links || *links != 1 || !reopened.matches_path(destination) || !reopened.lock_exclusive())
        return detail::file_failure<NativeFile>(FileJournalErrorCode::IoError);
    const auto size = reopened.size();
    if (!size || !reopened.seek(*size))
        return detail::file_failure<NativeFile>(FileJournalErrorCode::IoError);
    return runtime::Result<NativeFile, FileJournalError>(runtime::Ok(std::move(reopened)));
}

runtime::Result<bool, JournalSinkError> sink_failure(JournalSinkError error) {
    return runtime::Result<bool, JournalSinkError>(runtime::Err(error));
}

} // namespace

struct FileJournal::Impl {
    std::filesystem::path path;
    SchemaRegistry registry;
    FileJournalLimits limits;
    NativeFile lifetime_lock;
    NativeFile file;
    Project current_snapshot;
    DocumentRevision revision;
    bool failed = false;
};

FileJournal::FileJournal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FileJournal::~FileJournal() = default;

runtime::Result<FileJournalOpenResult, FileJournalError>
FileJournal::open(const std::filesystem::path& path, Project fallback, SchemaRegistry registry,
                  const FileJournalLimits& limits) {
    if (path.empty() || limits.max_record_bytes == 0 ||
        limits.max_file_bytes < detail::kFileHeaderBytes + detail::kFrameHeaderBytes)
        return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::LimitExceeded);

    std::filesystem::path canonical_path;
    if (!detail::resolve_journal_path(path, canonical_path))
        return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
    if (!detail::ensure_parent_directory(canonical_path))
        return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);

    auto lock_path = canonical_path;
    lock_path += ".lock";
    auto lifetime_lock = NativeFile::open_or_create(lock_path);
    if (!lifetime_lock.valid())
        return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
    if (!lifetime_lock.lock_exclusive())
        return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::AlreadyOpen);

    std::error_code exists_error;
    const auto exists = std::filesystem::exists(canonical_path, exists_error);
    if (exists_error)
        return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);

    NativeFile file;
    Project checkpoint = fallback;
    DocumentRevision revision{};
    bool repaired = false;
    if (exists) {
        file = NativeFile::open_existing(canonical_path);
        if (!file.valid())
            return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
        const auto links = file.link_count();
        if (!links)
            return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
        if (*links != 1)
            return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::AliasedPath);
        if (!file.matches_path(canonical_path))
            return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
        if (!file.lock_exclusive())
            return detail::file_failure<FileJournalOpenResult>(FileJournalErrorCode::AlreadyOpen);
        auto scanned = detail::scan_file(file, registry, limits);
        if (!scanned)
            return runtime::Result<FileJournalOpenResult, FileJournalError>(
                runtime::Err(std::move(scanned).error()));
        auto recovered = std::move(scanned).value();
        checkpoint = std::move(recovered.checkpoint);
        revision = recovered.revision;
        repaired = recovered.repaired_torn_tail;
    } else {
        auto initialized = write_checkpoint_file(canonical_path, fallback, {}, registry, limits);
        if (!initialized)
            return runtime::Result<FileJournalOpenResult, FileJournalError>(
                runtime::Err(std::move(initialized).error()));
        file = std::move(initialized).value();
    }

    auto impl = std::make_unique<Impl>(Impl{canonical_path, std::move(registry), limits,
                                            std::move(lifetime_lock), std::move(file), checkpoint,
                                            revision, false});
    auto sink = std::shared_ptr<FileJournal>(new FileJournal(std::move(impl)));
    return runtime::Result<FileJournalOpenResult, FileJournalError>(runtime::Ok(
        FileJournalOpenResult{sink, std::move(checkpoint), revision, exists, repaired}));
}

runtime::Result<bool, JournalSinkError>
FileJournal::append_batch(const JournalEntry& entry) noexcept {
    if (impl_->failed)
        return sink_failure(JournalSinkError::Closed);
    if (entry.before != impl_->revision ||
        impl_->revision.value == std::numeric_limits<std::uint64_t>::max() ||
        entry.after.value != impl_->revision.value + 1) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    const auto links = impl_->file.link_count();
    if (!links || *links != 1 || !impl_->file.matches_path(impl_->path)) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    auto reduced = detail::reduce_transaction(impl_->current_snapshot, entry.transaction,
                                              entry.kind == JournalEntryKind::History);
    if (!reduced) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    auto encoded = serialize_project(reduced.value().project, impl_->registry,
                                     SerializeOptions{impl_->limits.max_record_bytes});
    if (!encoded) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    const auto file_size = impl_->file.size();
    const auto payload_size = encoded.value().json.size();
    if (!file_size || payload_size > impl_->limits.max_file_bytes ||
        *file_size > impl_->limits.max_file_bytes - payload_size ||
        detail::kFrameHeaderBytes + detail::kCommitTrailerBytes >
            impl_->limits.max_file_bytes - *file_size - payload_size) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
    }
    if (!impl_->file.seek(*file_size) ||
        !detail::write_frame(impl_->file, detail::kCommitFrame, entry.after,
                             encoded.value().json) ||
        !impl_->file.sync()) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
    }
    const auto trailer = detail::commit_trailer(entry.after, *file_size);
    if (!impl_->file.write_all(trailer) || !impl_->file.sync()) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
    }
    const auto durable_links = impl_->file.link_count();
    if (!durable_links || *durable_links != 1 || !impl_->file.matches_path(impl_->path)) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
    }
    impl_->current_snapshot = std::move(reduced).value().project;
    impl_->revision = entry.after;
    return runtime::Result<bool, JournalSinkError>(runtime::Ok(true));
}

runtime::Result<bool, JournalSinkError>
FileJournal::checkpoint(const Project& snapshot, DocumentRevision durable_revision) noexcept {
    if (impl_->failed)
        return sink_failure(JournalSinkError::Closed);
    const auto links = impl_->file.link_count();
    if (!links || *links != 1 || !impl_->file.matches_path(impl_->path)) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    if (durable_revision.value > impl_->revision.value) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    if (durable_revision.value < impl_->revision.value)
        return runtime::Result<bool, JournalSinkError>(runtime::Ok(true));
    auto checkpoint = serialize_project(snapshot, impl_->registry,
                                        SerializeOptions{impl_->limits.max_record_bytes});
    auto durable = serialize_project(impl_->current_snapshot, impl_->registry,
                                     SerializeOptions{impl_->limits.max_record_bytes});
    if (!checkpoint || !durable || checkpoint.value().json != durable.value().json) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::InvalidState);
    }
    impl_->file.close();
    auto replacement = write_checkpoint_file(impl_->path, snapshot, durable_revision,
                                             impl_->registry, impl_->limits);
    if (!replacement) {
        const auto durability_uncertain =
            replacement.error().code == FileJournalErrorCode::DurabilityUncertain;
        impl_->file = NativeFile::open_existing(impl_->path);
        if (const auto size = impl_->file.size())
            (void)impl_->file.seek(*size);
        impl_->failed = true;
        return sink_failure(durability_uncertain ? JournalSinkError::DurabilityUncertain
                                                 : JournalSinkError::IoError);
    }
    impl_->file = std::move(replacement).value();
    impl_->current_snapshot = snapshot;
    impl_->revision = durable_revision;
    return runtime::Result<bool, JournalSinkError>(runtime::Ok(true));
}

runtime::Result<bool, JournalSinkError>
FileJournal::validate_restore(const Project& snapshot, DocumentRevision durable_revision) noexcept {
    if (impl_->failed)
        return sink_failure(JournalSinkError::Closed);
    const auto links = impl_->file.link_count();
    if (!links || *links != 1 || !impl_->file.matches_path(impl_->path) ||
        durable_revision != impl_->revision)
        return sink_failure(JournalSinkError::InvalidState);
    auto restored = serialize_project(snapshot, impl_->registry,
                                      SerializeOptions{impl_->limits.max_record_bytes});
    auto durable = serialize_project(impl_->current_snapshot, impl_->registry,
                                     SerializeOptions{impl_->limits.max_record_bytes});
    if (!restored || !durable || restored.value().json != durable.value().json)
        return sink_failure(JournalSinkError::InvalidState);
    return runtime::Result<bool, JournalSinkError>(runtime::Ok(true));
}

} // namespace pulp::timeline
