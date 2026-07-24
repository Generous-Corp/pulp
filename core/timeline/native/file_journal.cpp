#include <pulp/timeline/file_journal.hpp>

#include "../src/transaction_internal.hpp"
#include "file_journal_internal.hpp"

#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pulp::timeline {
namespace {

constexpr std::array<std::uint8_t, 8> kFileMagic{'P', 'T', 'L', 'J', 'N', 'L', '2', '\0'};
constexpr std::array<std::uint8_t, 4> kFrameMagic{'P', 'T', 'L', 'F'};
constexpr std::array<std::uint8_t, 4> kCommitMagic{'P', 'T', 'L', 'C'};
constexpr std::uint32_t kFileVersion = 2;
constexpr std::size_t kFileHeaderBytes = 16;
constexpr std::size_t kFrameHeaderBytes = 32;
constexpr std::size_t kCommitTrailerBytes = 24;
constexpr std::uint8_t kCheckpointFrame = 1;
constexpr std::uint8_t kCommitFrame = 2;

std::atomic<std::uint64_t> g_temporary_serial{0};
std::atomic<std::size_t> g_test_write_chunk{0};
std::atomic<std::size_t> g_test_written_bytes{0};
std::atomic<detail::FileJournalWriteHook> g_test_write_hook{nullptr};

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

class NativeFile {
  public:
    NativeFile() = default;
    explicit NativeFile(int descriptor) : descriptor_(descriptor) {}
    ~NativeFile() {
        close();
    }
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    NativeFile(NativeFile&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    NativeFile& operator=(NativeFile&& other) noexcept {
        if (this != &other) {
            close();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    static NativeFile open_existing(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
        return NativeFile(_wopen(path.c_str(), _O_RDWR | _O_BINARY | _O_NOINHERIT));
#else
        return NativeFile(::open(path.c_str(), O_RDWR | O_CLOEXEC));
#endif
    }

    static NativeFile create_exclusive(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
        return NativeFile(_wopen(path.c_str(),
                                 _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_NOINHERIT,
                                 _S_IREAD | _S_IWRITE));
#else
        return NativeFile(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600));
#endif
    }

    static NativeFile open_or_create(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
        return NativeFile(_wopen(path.c_str(), _O_CREAT | _O_RDWR | _O_BINARY | _O_NOINHERIT,
                                 _S_IREAD | _S_IWRITE));
#else
        return NativeFile(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
#endif
    }

    bool valid() const noexcept {
        return descriptor_ >= 0;
    }

    void close() noexcept {
        if (!valid())
            return;
#if defined(_WIN32)
        _close(descriptor_);
#else
        ::close(descriptor_);
#endif
        descriptor_ = -1;
    }

    bool seek(std::uint64_t offset) noexcept {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return false;
#if defined(_WIN32)
        return _lseeki64(descriptor_, static_cast<__int64>(offset), SEEK_SET) >= 0;
#else
        return ::lseek(descriptor_, static_cast<off_t>(offset), SEEK_SET) >= 0;
#endif
    }

    bool read_all(std::span<std::uint8_t> output) noexcept {
        std::size_t consumed = 0;
        while (consumed < output.size()) {
            const auto remaining = output.size() - consumed;
            const auto request =
                static_cast<unsigned int>(std::min<std::size_t>(remaining, 1u << 30));
#if defined(_WIN32)
            const auto count = _read(descriptor_, output.data() + consumed, request);
#else
            const auto count = ::read(descriptor_, output.data() + consumed, request);
#endif
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            consumed += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool write_all(std::span<const std::uint8_t> input) noexcept {
        std::size_t consumed = 0;
        while (consumed < input.size()) {
            const auto remaining = input.size() - consumed;
            auto maximum = std::size_t{1} << 30;
            const auto test_chunk = g_test_write_chunk.load(std::memory_order_relaxed);
            if (test_chunk != 0)
                maximum = std::min(maximum, test_chunk);
            const auto request =
                static_cast<unsigned int>(std::min<std::size_t>(remaining, maximum));
#if defined(_WIN32)
            const auto count = _write(descriptor_, input.data() + consumed, request);
#else
            const auto count = ::write(descriptor_, input.data() + consumed, request);
#endif
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            consumed += static_cast<std::size_t>(count);
            if (const auto hook = g_test_write_hook.load(std::memory_order_relaxed)) {
                const auto total = g_test_written_bytes.fetch_add(static_cast<std::size_t>(count),
                                                                  std::memory_order_relaxed) +
                                   static_cast<std::size_t>(count);
                hook(total);
            }
        }
        return true;
    }

    std::optional<std::uint64_t> size() const noexcept {
#if defined(_WIN32)
        struct _stat64 status{};
        if (_fstat64(descriptor_, &status) != 0 || status.st_size < 0)
            return std::nullopt;
#else
        struct stat status{};
        if (::fstat(descriptor_, &status) != 0 || status.st_size < 0)
            return std::nullopt;
#endif
        return static_cast<std::uint64_t>(status.st_size);
    }

    std::optional<std::uint64_t> link_count() const noexcept {
#if defined(_WIN32)
        struct _stat64 status{};
        if (_fstat64(descriptor_, &status) != 0)
            return std::nullopt;
#else
        struct stat status{};
        if (::fstat(descriptor_, &status) != 0)
            return std::nullopt;
#endif
        return static_cast<std::uint64_t>(status.st_nlink);
    }

    bool matches_path(const std::filesystem::path& path) const noexcept {
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION held{};
        const auto held_handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor_));
        if (held_handle == INVALID_HANDLE_VALUE ||
            ::GetFileInformationByHandle(held_handle, &held) == 0)
            return false;
        const auto named_handle =
            ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (named_handle == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION named{};
        const auto queried = ::GetFileInformationByHandle(named_handle, &named) != 0;
        ::CloseHandle(named_handle);
        return queried && held.dwVolumeSerialNumber == named.dwVolumeSerialNumber &&
               held.nFileIndexHigh == named.nFileIndexHigh &&
               held.nFileIndexLow == named.nFileIndexLow;
#else
        struct stat held{};
        struct stat named{};
        return ::fstat(descriptor_, &held) == 0 && ::stat(path.c_str(), &named) == 0 &&
               held.st_dev == named.st_dev && held.st_ino == named.st_ino;
#endif
    }

    bool truncate(std::uint64_t size) noexcept {
#if defined(_WIN32)
        return _chsize_s(descriptor_, size) == 0;
#else
        if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            return false;
        return ::ftruncate(descriptor_, static_cast<off_t>(size)) == 0;
#endif
    }

    bool sync() noexcept {
#if defined(_WIN32)
        return _commit(descriptor_) == 0;
#elif defined(__APPLE__) && defined(F_FULLFSYNC)
        if (::fcntl(descriptor_, F_FULLFSYNC) == 0)
            return true;
        if (errno != ENOTSUP)
            return false;
        return ::fsync(descriptor_) == 0;
#else
        return ::fsync(descriptor_) == 0;
#endif
    }

    bool lock_exclusive() noexcept {
#if defined(_WIN32)
        OVERLAPPED overlap{};
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor_));
        return handle != INVALID_HANDLE_VALUE &&
               ::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                            &overlap) != 0;
#else
        return ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0;
#endif
    }

  private:
    int descriptor_ = -1;
};

bool sync_directory(const std::filesystem::path& directory) noexcept {
#if defined(_WIN32)
    (void)directory;
    return true;
#else
    const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    const auto result = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return result;
#endif
}

bool sync_parent_directory(const std::filesystem::path& path) noexcept {
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    return sync_directory(parent);
}

bool ensure_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty())
        return true;

    std::vector<std::filesystem::path> missing;
    auto cursor = parent;
    std::error_code error;
    while (!cursor.empty() && !std::filesystem::exists(cursor, error)) {
        if (error)
            return false;
        missing.push_back(cursor);
        cursor = cursor.parent_path();
    }
    if (error)
        return false;
    std::filesystem::create_directories(parent, error);
    if (error)
        return false;

    for (const auto& directory : missing) {
        if (!sync_directory(directory))
            return false;
    }
    if (!missing.empty()) {
        auto existing_parent = missing.back().parent_path();
        if (existing_parent.empty())
            existing_parent = ".";
        if (!sync_directory(existing_parent))
            return false;
    }
    return true;
}

std::array<std::uint8_t, kFileHeaderBytes> file_header() noexcept {
    std::array<std::uint8_t, kFileHeaderBytes> header{};
    std::copy(kFileMagic.begin(), kFileMagic.end(), header.begin());
    write_u32(header.data() + 8, kFileVersion);
    write_u32(header.data() + 12, static_cast<std::uint32_t>(kFileHeaderBytes));
    return header;
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

bool valid_commit_trailer(std::span<const std::uint8_t, kCommitTrailerBytes> trailer,
                          DocumentRevision revision, std::uint64_t frame_offset) noexcept {
    return std::equal(kCommitMagic.begin(), kCommitMagic.end(), trailer.begin()) &&
           read_u64(trailer.data() + 4) == revision.value &&
           read_u64(trailer.data() + 12) == frame_offset &&
           read_u32(trailer.data() + 20) ==
               crc32(std::span<const std::uint8_t>(trailer.data(), trailer.size() - 4));
}

bool replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination) noexcept {
#if defined(_WIN32)
    return ::MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}

std::filesystem::path temporary_sibling(const std::filesystem::path& destination) {
#if defined(_WIN32)
    const auto process = static_cast<std::uint64_t>(_getpid());
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    auto path = destination;
    path += ".tmp." + std::to_string(process) + "." +
            std::to_string(g_temporary_serial.fetch_add(1, std::memory_order_relaxed));
    return path;
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

bool write_frame(NativeFile& file, std::uint8_t kind, DocumentRevision revision,
                 std::string_view payload) noexcept {
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    const auto header = frame_header(kind, revision, bytes);
    return file.write_all(header) && file.write_all(bytes);
}

runtime::Result<NativeFile, FileJournalError>
write_checkpoint_file(const std::filesystem::path& destination, const Project& snapshot,
                      DocumentRevision revision, const SchemaRegistry& registry,
                      const FileJournalLimits& limits) {
    auto encoded = encode_snapshot(snapshot, registry, limits, kFileHeaderBytes);
    if (!encoded)
        return runtime::Result<NativeFile, FileJournalError>(
            runtime::Err(std::move(encoded).error()));
    const auto fixed_bytes = kFileHeaderBytes + kFrameHeaderBytes;
    if (limits.max_file_bytes < fixed_bytes ||
        encoded.value().size() > limits.max_file_bytes - fixed_bytes)
        return file_failure<NativeFile>(FileJournalErrorCode::LimitExceeded);

    std::filesystem::path temporary;
    NativeFile file;
    for (std::size_t attempt = 0; attempt < 128 && !file.valid(); ++attempt) {
        temporary = temporary_sibling(destination);
        file = NativeFile::create_exclusive(temporary);
        if (!file.valid() && errno != EEXIST)
            break;
    }
    if (!file.valid())
        return file_failure<NativeFile>(FileJournalErrorCode::IoError);
    const auto header = file_header();
    if (!file.write_all(header) ||
        !write_frame(file, kCheckpointFrame, revision, encoded.value()) || !file.sync()) {
        file.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return file_failure<NativeFile>(FileJournalErrorCode::IoError);
    }
    file.close();
    if (!replace_file(temporary, destination) || !sync_parent_directory(destination)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return file_failure<NativeFile>(FileJournalErrorCode::IoError);
    }
    auto reopened = NativeFile::open_existing(destination);
    if (!reopened.valid())
        return file_failure<NativeFile>(FileJournalErrorCode::IoError);
    const auto links = reopened.link_count();
    if (!links || *links != 1 || !reopened.matches_path(destination) ||
        !reopened.lock_exclusive()) {
        return file_failure<NativeFile>(FileJournalErrorCode::IoError);
    }
    const auto size = reopened.size();
    if (!size || !reopened.seek(*size))
        return file_failure<NativeFile>(FileJournalErrorCode::IoError);
    return runtime::Result<NativeFile, FileJournalError>(runtime::Ok(std::move(reopened)));
}

struct ScannedFile {
    Project checkpoint;
    DocumentRevision revision;
    std::uint64_t valid_bytes = 0;
    bool repaired_torn_tail = false;
};

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
            read_u32(frame.data() + 28) != header_crc)
            return file_failure<ScannedFile>(FileJournalErrorCode::CorruptRecord, offset);
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
        limits.max_file_bytes < kFileHeaderBytes + kFrameHeaderBytes)
        return file_failure<FileJournalOpenResult>(FileJournalErrorCode::LimitExceeded);

    if (!ensure_parent_directory(path))
        return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);

    std::error_code canonical_error;
    const auto canonical_path = std::filesystem::weakly_canonical(path, canonical_error);
    if (canonical_error)
        return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);

    auto lock_path = canonical_path;
    lock_path += ".lock";
    auto lifetime_lock = NativeFile::open_or_create(lock_path);
    if (!lifetime_lock.valid())
        return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
    if (!lifetime_lock.lock_exclusive())
        return file_failure<FileJournalOpenResult>(FileJournalErrorCode::AlreadyOpen);

    std::error_code exists_error;
    const auto exists = std::filesystem::exists(canonical_path, exists_error);
    if (exists_error)
        return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);

    NativeFile file;
    Project checkpoint = fallback;
    DocumentRevision revision{};
    bool repaired = false;
    if (exists) {
        file = NativeFile::open_existing(canonical_path);
        if (!file.valid())
            return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
        const auto links = file.link_count();
        if (!links)
            return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
        if (*links != 1)
            return file_failure<FileJournalOpenResult>(FileJournalErrorCode::AliasedPath);
        if (!file.matches_path(canonical_path))
            return file_failure<FileJournalOpenResult>(FileJournalErrorCode::IoError);
        if (!file.lock_exclusive())
            return file_failure<FileJournalOpenResult>(FileJournalErrorCode::AlreadyOpen);
        auto scanned = scan_file(file, registry, limits);
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
        kFrameHeaderBytes + kCommitTrailerBytes >
            impl_->limits.max_file_bytes - *file_size - payload_size) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
    }
    if (!impl_->file.seek(*file_size) ||
        !write_frame(impl_->file, kCommitFrame, entry.after, encoded.value().json) ||
        !impl_->file.sync()) {
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
    }
    const auto trailer = commit_trailer(entry.after, *file_size);
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
        impl_->file = NativeFile::open_existing(impl_->path);
        if (const auto size = impl_->file.size())
            (void)impl_->file.seek(*size);
        impl_->failed = true;
        return sink_failure(JournalSinkError::IoError);
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

void detail::FileJournalTestAccess::set_write_hook(std::size_t maximum_chunk,
                                                   FileJournalWriteHook hook) noexcept {
    g_test_written_bytes.store(0, std::memory_order_relaxed);
    g_test_write_chunk.store(maximum_chunk, std::memory_order_relaxed);
    g_test_write_hook.store(hook, std::memory_order_release);
}

void detail::FileJournalTestAccess::clear_write_hook() noexcept {
    g_test_write_hook.store(nullptr, std::memory_order_release);
    g_test_write_chunk.store(0, std::memory_order_relaxed);
    g_test_written_bytes.store(0, std::memory_order_relaxed);
}

} // namespace pulp::timeline
