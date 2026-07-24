#include "file_journal_codec.hpp"

#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

namespace pulp::timeline::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kFileMagic{'P', 'T', 'L', 'J', 'N', 'L', '2', '\0'};
constexpr std::array<std::uint8_t, 4> kFrameMagic{'P', 'T', 'L', 'F'};
constexpr std::array<std::uint8_t, 4> kCommitMagic{'P', 'T', 'L', 'C'};
constexpr std::uint32_t kFileVersion = 2;

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = (crc & 1u) ? 0xffffffffu : 0u;
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void write_u32(std::uint8_t* output, std::uint32_t value) noexcept {
    for (std::size_t byte = 0; byte < 4; ++byte)
        output[byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

void write_u64(std::uint8_t* output, std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < 8; ++byte)
        output[byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

std::uint32_t read_u32(const std::uint8_t* input) noexcept {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte)
        value |= static_cast<std::uint32_t>(input[byte]) << (byte * 8);
    return value;
}

std::uint64_t read_u64(const std::uint8_t* input) noexcept {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8; ++byte)
        value |= static_cast<std::uint64_t>(input[byte]) << (byte * 8);
    return value;
}

std::array<std::uint8_t, kFrameHeaderBytes>
frame_header(std::uint8_t kind, DocumentRevision revision,
             std::span<const std::uint8_t> payload) noexcept {
    std::array<std::uint8_t, kFrameHeaderBytes> header{};
    std::copy(kFrameMagic.begin(), kFrameMagic.end(), header.begin());
    header[4] = kind;
    write_u64(header.data() + 8, revision.value);
    write_u64(header.data() + 16, payload.size());
    write_u32(header.data() + 24, crc32(payload));
    write_u32(header.data() + 28,
              crc32(std::span<const std::uint8_t>(header.data(), header.size() - 4)));
    return header;
}

bool valid_commit_trailer(std::span<const std::uint8_t, kCommitTrailerBytes> trailer,
                          DocumentRevision revision, std::uint64_t frame_offset) noexcept {
    return std::equal(kCommitMagic.begin(), kCommitMagic.end(), trailer.begin()) &&
           read_u64(trailer.data() + 4) == revision.value &&
           read_u64(trailer.data() + 12) == frame_offset &&
           read_u32(trailer.data() + 20) ==
               crc32(std::span<const std::uint8_t>(trailer.data(), trailer.size() - 4));
}

template <typename File>
bool write_frame_to(File& file, std::uint8_t kind, DocumentRevision revision,
                    std::string_view payload) noexcept {
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    const auto header = frame_header(kind, revision, bytes);
    return file.write_all(header) && file.write_all(bytes);
}

} // namespace

std::array<std::uint8_t, kFileHeaderBytes> file_header() noexcept {
    std::array<std::uint8_t, kFileHeaderBytes> header{};
    std::copy(kFileMagic.begin(), kFileMagic.end(), header.begin());
    write_u32(header.data() + 8, kFileVersion);
    write_u32(header.data() + 12, static_cast<std::uint32_t>(kFileHeaderBytes));
    return header;
}

std::array<std::uint8_t, kCommitTrailerBytes> commit_trailer(DocumentRevision revision,
                                                             std::uint64_t frame_offset) noexcept {
    std::array<std::uint8_t, kCommitTrailerBytes> trailer{};
    std::copy(kCommitMagic.begin(), kCommitMagic.end(), trailer.begin());
    write_u64(trailer.data() + 4, revision.value);
    write_u64(trailer.data() + 12, frame_offset);
    write_u32(trailer.data() + 20,
              crc32(std::span<const std::uint8_t>(trailer.data(), trailer.size() - 4)));
    return trailer;
}

bool write_frame(NativeFile& file, std::uint8_t kind, DocumentRevision revision,
                 std::string_view payload) noexcept {
    return write_frame_to(file, kind, revision, payload);
}

bool write_frame(runtime::detail::DurableFileReplacement& file, std::uint8_t kind,
                 DocumentRevision revision, std::string_view payload) noexcept {
    return write_frame_to(file, kind, revision, payload);
}

runtime::Result<std::string, FileJournalError> encode_snapshot(const Project& snapshot,
                                                               const SchemaRegistry& registry,
                                                               const FileJournalLimits& limits,
                                                               std::uint64_t offset) {
    auto encoded = serialize_project(snapshot, registry, SerializeOptions{limits.max_record_bytes});
    if (!encoded)
        return persistence_failure<std::string>(encoded.error(), offset);
    return runtime::Result<std::string, FileJournalError>(
        runtime::Ok(std::move(encoded).value().json));
}

runtime::Result<ScannedFile, FileJournalError>
scan_file(NativeFile& file, const SchemaRegistry& registry, const FileJournalLimits& limits) {
    const auto size = file.size();
    if (!size)
        return file_failure<ScannedFile>(FileJournalErrorCode::IoError);
    if (*size > limits.max_file_bytes)
        return file_failure<ScannedFile>(FileJournalErrorCode::LimitExceeded);
    if (*size < kFileHeaderBytes)
        return file_failure<ScannedFile>(FileJournalErrorCode::InvalidFormat);
    if (!file.seek(0))
        return file_failure<ScannedFile>(FileJournalErrorCode::IoError);
    std::array<std::uint8_t, kFileHeaderBytes> header{};
    if (!file.read_all(header))
        return file_failure<ScannedFile>(FileJournalErrorCode::IoError);
    if (!std::equal(kFileMagic.begin(), kFileMagic.end(), header.begin()) ||
        read_u32(header.data() + 12) != kFileHeaderBytes)
        return file_failure<ScannedFile>(FileJournalErrorCode::InvalidFormat);
    if (read_u32(header.data() + 8) != kFileVersion)
        return file_failure<ScannedFile>(FileJournalErrorCode::UnsupportedVersion);

    std::optional<Project> latest;
    DocumentRevision revision{};
    std::uint64_t offset = kFileHeaderBytes;
    bool torn_tail = false;
    while (offset < *size) {
        if (*size - offset < kFrameHeaderBytes) {
            torn_tail = true;
            break;
        }
        std::array<std::uint8_t, kFrameHeaderBytes> frame{};
        if (!file.seek(offset) || !file.read_all(frame))
            return file_failure<ScannedFile>(FileJournalErrorCode::IoError, offset);
        const auto header_crc =
            crc32(std::span<const std::uint8_t>(frame.data(), frame.size() - 4));
        if (!std::equal(kFrameMagic.begin(), kFrameMagic.end(), frame.begin()) ||
            read_u32(frame.data() + 28) != header_crc) {
            // A commit trailer is written only after the complete frame is durable.
            // A header-sized final tail therefore cannot contain a committed frame.
            if (*size - offset == kFrameHeaderBytes) {
                torn_tail = true;
                break;
            }
            return file_failure<ScannedFile>(FileJournalErrorCode::CorruptRecord, offset);
        }
        const auto kind = frame[4];
        const auto frame_revision = DocumentRevision{read_u64(frame.data() + 8)};
        const auto payload_size = read_u64(frame.data() + 16);
        if ((kind != kCheckpointFrame && kind != kCommitFrame) ||
            payload_size > limits.max_record_bytes)
            return file_failure<ScannedFile>(FileJournalErrorCode::InvalidFormat, offset);
        if ((!latest && kind != kCheckpointFrame) ||
            (latest &&
             (kind != kCommitFrame || revision.value == std::numeric_limits<std::uint64_t>::max() ||
              frame_revision.value != revision.value + 1)))
            return file_failure<ScannedFile>(FileJournalErrorCode::RevisionMismatch, offset);
        if (payload_size > std::numeric_limits<std::uint64_t>::max() - kFrameHeaderBytes - offset)
            return file_failure<ScannedFile>(FileJournalErrorCode::LimitExceeded, offset);
        const auto frame_end = offset + kFrameHeaderBytes + payload_size;
        if (frame_end > *size) {
            torn_tail = true;
            break;
        }
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size));
        if (!file.read_all(payload))
            return file_failure<ScannedFile>(FileJournalErrorCode::IoError, offset);
        auto committed_end = frame_end;
        if (kind == kCommitFrame) {
            if (frame_end > std::numeric_limits<std::uint64_t>::max() - kCommitTrailerBytes)
                return file_failure<ScannedFile>(FileJournalErrorCode::LimitExceeded, offset);
            const auto trailer_end = frame_end + kCommitTrailerBytes;
            if (trailer_end > *size) {
                torn_tail = true;
                break;
            }
            std::array<std::uint8_t, kCommitTrailerBytes> trailer{};
            if (!file.read_all(trailer))
                return file_failure<ScannedFile>(FileJournalErrorCode::IoError, offset);
            if (!valid_commit_trailer(trailer, frame_revision, offset)) {
                if (trailer_end == *size) {
                    torn_tail = true;
                    break;
                }
                return file_failure<ScannedFile>(FileJournalErrorCode::CorruptRecord, offset);
            }
            committed_end = trailer_end;
        }
        if (crc32(payload) != read_u32(frame.data() + 24))
            return file_failure<ScannedFile>(FileJournalErrorCode::CorruptRecord, offset);
        const auto json =
            std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size());
        auto decoded = deserialize_project(json, registry, limits.decode);
        if (!decoded)
            return persistence_failure<ScannedFile>(decoded.error(), offset);
        latest = std::move(decoded).value();
        revision = frame_revision;
        offset = committed_end;
    }
    if (!latest)
        return file_failure<ScannedFile>(FileJournalErrorCode::InvalidFormat, offset);
    if (torn_tail) {
        if (!file.truncate(offset) || !file.sync())
            return file_failure<ScannedFile>(FileJournalErrorCode::IoError, offset);
    }
    if (!file.seek(offset))
        return file_failure<ScannedFile>(FileJournalErrorCode::IoError, offset);
    return runtime::Result<ScannedFile, FileJournalError>(
        runtime::Ok(ScannedFile{std::move(*latest), revision, offset, torn_tail}));
}

} // namespace pulp::timeline::detail
