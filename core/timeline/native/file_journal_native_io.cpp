#include "file_journal_native_io.hpp"

#include "file_journal_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
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
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pulp::timeline::detail {
namespace {

std::atomic<std::size_t> g_test_write_chunk{0};
std::atomic<std::size_t> g_test_written_bytes{0};
std::atomic<FileJournalWriteHook> g_test_write_hook{nullptr};

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

} // namespace

NativeFile::NativeFile(int descriptor) noexcept : descriptor_(descriptor) {}

NativeFile::~NativeFile() {
    close();
}

NativeFile::NativeFile(NativeFile&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

NativeFile& NativeFile::operator=(NativeFile&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

NativeFile NativeFile::open_existing(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    return NativeFile(_wopen(path.c_str(), _O_RDWR | _O_BINARY | _O_NOINHERIT));
#else
    return NativeFile(::open(path.c_str(), O_RDWR | O_CLOEXEC));
#endif
}

NativeFile NativeFile::open_or_create(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    return NativeFile(
        _wopen(path.c_str(), _O_CREAT | _O_RDWR | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE));
#else
    return NativeFile(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
#endif
}

bool NativeFile::valid() const noexcept {
    return descriptor_ >= 0;
}

int NativeFile::native_descriptor() const noexcept {
    return descriptor_;
}

void NativeFile::close() noexcept {
    if (!valid())
        return;
#if defined(_WIN32)
    _close(descriptor_);
#else
    ::close(descriptor_);
#endif
    descriptor_ = -1;
}

bool NativeFile::seek(std::uint64_t offset) noexcept {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return false;
#if defined(_WIN32)
    return _lseeki64(descriptor_, static_cast<__int64>(offset), SEEK_SET) >= 0;
#else
    return ::lseek(descriptor_, static_cast<off_t>(offset), SEEK_SET) >= 0;
#endif
}

bool NativeFile::read_all(std::span<std::uint8_t> output) noexcept {
    std::size_t consumed = 0;
    while (consumed < output.size()) {
        const auto remaining = output.size() - consumed;
        const auto request =
            static_cast<unsigned int>(std::min<std::size_t>(remaining, std::size_t{1} << 30));
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

bool NativeFile::write_all(std::span<const std::uint8_t> input) noexcept {
    std::size_t consumed = 0;
    while (consumed < input.size()) {
        const auto remaining = input.size() - consumed;
        auto maximum = std::size_t{1} << 30;
        const auto test_chunk = g_test_write_chunk.load(std::memory_order_relaxed);
        if (test_chunk != 0)
            maximum = std::min(maximum, test_chunk);
        const auto request = static_cast<unsigned int>(std::min<std::size_t>(remaining, maximum));
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

std::optional<std::uint64_t> NativeFile::size() const noexcept {
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

std::optional<std::uint64_t> NativeFile::link_count() const noexcept {
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

bool NativeFile::matches_path(const std::filesystem::path& path) const noexcept {
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION held{};
    const auto held_handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor_));
    if (held_handle == INVALID_HANDLE_VALUE ||
        ::GetFileInformationByHandle(held_handle, &held) == 0)
        return false;
    const auto named_handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (named_handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION named{};
    const auto queried = ::GetFileInformationByHandle(named_handle, &named) != 0;
    ::CloseHandle(named_handle);
    return queried && held.dwVolumeSerialNumber == named.dwVolumeSerialNumber &&
           held.nFileIndexHigh == named.nFileIndexHigh && held.nFileIndexLow == named.nFileIndexLow;
#else
    struct stat held{};
    struct stat named{};
    return ::fstat(descriptor_, &held) == 0 && ::stat(path.c_str(), &named) == 0 &&
           held.st_dev == named.st_dev && held.st_ino == named.st_ino;
#endif
}

bool NativeFile::truncate(std::uint64_t size) noexcept {
#if defined(_WIN32)
    return _chsize_s(descriptor_, size) == 0;
#else
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    return ::ftruncate(descriptor_, static_cast<off_t>(size)) == 0;
#endif
}

bool NativeFile::sync() noexcept {
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

bool NativeFile::lock_exclusive() noexcept {
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

bool resolve_journal_path(const std::filesystem::path& requested, std::filesystem::path& resolved) {
    std::error_code error;
    auto candidate = std::filesystem::absolute(requested, error);
    if (error)
        return false;
    candidate = candidate.lexically_normal();

    // Resolve a dangling final symlink explicitly so initialization creates its
    // target and uses the same lock identity as a later open through that target.
    constexpr std::size_t kMaximumSymlinkDepth = 64;
    for (std::size_t depth = 0; depth < kMaximumSymlinkDepth; ++depth) {
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error) {
            if (error != std::errc::no_such_file_or_directory)
                return false;
            error.clear();
        }
        if (!std::filesystem::is_symlink(status)) {
            resolved = std::filesystem::weakly_canonical(candidate, error);
            return !error;
        }

        auto target = std::filesystem::read_symlink(candidate, error);
        if (error)
            return false;
        if (target.is_relative())
            target = candidate.parent_path() / target;
        candidate = target.lexically_normal();
    }
    return false;
}

void FileJournalTestAccess::set_write_hook(std::size_t maximum_chunk,
                                           FileJournalWriteHook hook) noexcept {
    g_test_written_bytes.store(0, std::memory_order_relaxed);
    g_test_write_chunk.store(maximum_chunk, std::memory_order_relaxed);
    g_test_write_hook.store(hook, std::memory_order_release);
}

void FileJournalTestAccess::clear_write_hook() noexcept {
    g_test_write_hook.store(nullptr, std::memory_order_release);
    g_test_write_chunk.store(0, std::memory_order_relaxed);
    g_test_written_bytes.store(0, std::memory_order_relaxed);
}

} // namespace pulp::timeline::detail
