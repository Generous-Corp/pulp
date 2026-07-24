#pragma once

#include "file_journal_native_io.hpp"

#include <pulp/runtime/detail/durable_file_replacement.hpp>
#include <pulp/timeline/file_journal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::timeline::detail {

inline constexpr std::size_t kFileHeaderBytes = 16;
inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::size_t kCommitTrailerBytes = 24;
inline constexpr std::uint8_t kCheckpointFrame = 1;
inline constexpr std::uint8_t kCommitFrame = 2;

template <typename T>
runtime::Result<T, FileJournalError> file_failure(FileJournalErrorCode code,
                                                  std::uint64_t offset = 0) {
    return runtime::Result<T, FileJournalError>(
        runtime::Err(FileJournalError{code, offset, std::nullopt}));
}

template <typename T>
runtime::Result<T, FileJournalError> persistence_failure(PersistenceError error,
                                                         std::uint64_t offset) {
    FileJournalError value;
    value.code = FileJournalErrorCode::PersistenceError;
    value.byte_offset = offset;
    value.persistence_error = std::move(error);
    return runtime::Result<T, FileJournalError>(runtime::Err(std::move(value)));
}

std::array<std::uint8_t, kFileHeaderBytes> file_header() noexcept;
std::array<std::uint8_t, kCommitTrailerBytes> commit_trailer(DocumentRevision revision,
                                                             std::uint64_t frame_offset) noexcept;
bool write_frame(NativeFile& file, std::uint8_t kind, DocumentRevision revision,
                 std::string_view payload) noexcept;
bool write_frame(runtime::detail::DurableFileReplacement& file, std::uint8_t kind,
                 DocumentRevision revision, std::string_view payload) noexcept;

runtime::Result<std::string, FileJournalError> encode_snapshot(const Project& snapshot,
                                                               const SchemaRegistry& registry,
                                                               const FileJournalLimits& limits,
                                                               std::uint64_t offset);

struct ScannedFile {
    Project checkpoint;
    DocumentRevision revision;
    std::uint64_t valid_bytes = 0;
    bool repaired_torn_tail = false;
};

runtime::Result<ScannedFile, FileJournalError>
scan_file(NativeFile& file, const SchemaRegistry& registry, const FileJournalLimits& limits);

} // namespace pulp::timeline::detail
