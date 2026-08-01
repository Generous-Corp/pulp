#include "native_io.hpp"

#include "project_package_test_access.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace pulp::project_package::detail {

namespace {

#if defined(_WIN32)
bool write_all(HANDLE handle, std::span<const std::uint8_t> bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<std::size_t>(0x7ffff000u)));
        DWORD written = 0;
        if (!::WriteFile(handle, bytes.data() + offset, request, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}
#else
bool write_all(int descriptor, std::span<const std::uint8_t> bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool fence_descriptor(int descriptor) noexcept {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0)
        return true;
    if (errno != ENOTSUP)
        return false;
#endif
    return ::fsync(descriptor) == 0;
}
#endif

} // namespace

bool write_exclusive_and_fence(const std::filesystem::path& path,
                               std::span<const std::uint8_t> bytes, PackageFaultPoint written_point,
                               PackageFaultPoint fenced_point) noexcept {
#if defined(_WIN32)
    const auto handle =
        ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool written = write_all(handle, bytes);
    if (written)
        invoke_fault_hook(written_point);
    const bool fenced = written && ::FlushFileBuffers(handle) != 0;
    if (fenced)
        invoke_fault_hook(fenced_point);
    const bool closed = ::CloseHandle(handle) != 0;
    return fenced && closed;
#else
    const auto descriptor = ::open(path.c_str(),
                                   O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                       | O_NOFOLLOW
#endif
                                   ,
                                   0600);
    if (descriptor < 0)
        return false;
    const bool written = write_all(descriptor, bytes);
    if (written)
        invoke_fault_hook(written_point);
    const bool fenced = written && fence_descriptor(descriptor);
    if (fenced)
        invoke_fault_hook(fenced_point);
    const bool closed = ::close(descriptor) == 0;
    return fenced && closed;
#endif
}

bool fence_file(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    const auto handle =
        ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool fenced = ::FlushFileBuffers(handle) != 0;
    const bool closed = ::CloseHandle(handle) != 0;
    return fenced && closed;
#else
    const auto descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC
#ifdef O_NOFOLLOW
                                                     | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return false;
    const bool fenced = fence_descriptor(descriptor);
    const bool closed = ::close(descriptor) == 0;
    return fenced && closed;
#endif
}

bool fence_directory(const std::filesystem::path& directory) noexcept {
#if defined(_WIN32)
    // Windows exposes write-through rename as its supported namespace fence;
    // directory handles cannot portably be passed to FlushFileBuffers.
    (void)directory;
    return true;
#else
    const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                          | O_DIRECTORY
#endif
    );
    if (descriptor < 0)
        return false;
    const bool fenced = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    return fenced && closed;
#endif
}

bool publish_no_replace(const std::filesystem::path& source,
                        const std::filesystem::path& destination) noexcept {
#if defined(_WIN32)
    return ::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__APPLE__)
    return ::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL)
        return false;
    if (::link(source.c_str(), destination.c_str()) != 0)
        return false;
    // The destination becomes visible atomically when link() succeeds. Failure
    // to remove the private source is cleanup debt, not a failed publication;
    // reporting false here would let callers retry after publication.
    (void)::unlink(source.c_str());
    return true;
#else
    (void)source;
    (void)destination;
    return false;
#endif
}

bool replace_path(const std::filesystem::path& source,
                  const std::filesystem::path& destination) noexcept {
#if defined(_WIN32)
    return ::MoveFileExW(source.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

bool regular_file_no_links(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    const auto handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid =
        ::GetFileInformationByHandle(handle, &info) != 0 &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        info.nNumberOfLinks == 1;
    ::CloseHandle(handle);
    return valid;
#else
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 1;
#endif
}

NativeReadOutcome read_file_bounded(const std::filesystem::path& path, std::uint64_t maximum_bytes,
                                    std::vector<std::uint8_t>& bytes) noexcept {
    bytes.clear();
#if defined(_WIN32)
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return NativeReadOutcome::IoError;
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks > 1) {
        ::CloseHandle(handle);
        return NativeReadOutcome::InvalidFile;
    }
    const auto size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32u) | info.nFileSizeLow;
    if (size > maximum_bytes || size > static_cast<std::uint64_t>(SIZE_MAX)) {
        ::CloseHandle(handle);
        return NativeReadOutcome::LimitExceeded;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<std::size_t>(0x7ffff000u)));
        DWORD count = 0;
        if (!::ReadFile(handle, bytes.data() + offset, request, &count, nullptr) || count == 0) {
            ::CloseHandle(handle);
            bytes.clear();
            return NativeReadOutcome::IoError;
        }
        offset += count;
    }
    if (::CloseHandle(handle) == 0) {
        bytes.clear();
        return NativeReadOutcome::IoError;
    }
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                                     | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return NativeReadOutcome::IoError;
    struct stat status{};
    // A concurrent atomic replacement may unlink the old generation after we
    // opened it, reducing this handle's link count to zero. That handle remains
    // a complete, valid old snapshot. Reject aliases (>1), but admit the
    // ordinary linked (1) and concurrently unlinked (0) states.
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink > 1) {
        ::close(descriptor);
        return NativeReadOutcome::InvalidFile;
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > maximum_bytes ||
        static_cast<std::uint64_t>(status.st_size) > static_cast<std::uint64_t>(SIZE_MAX)) {
        ::close(descriptor);
        return NativeReadOutcome::LimitExceeded;
    }
    bytes.resize(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(descriptor);
            bytes.clear();
            return NativeReadOutcome::IoError;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        bytes.clear();
        return NativeReadOutcome::IoError;
    }
#endif
    return NativeReadOutcome::Ok;
}

} // namespace pulp::project_package::detail
