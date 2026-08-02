#include "native_io.hpp"

#include "project_package_test_access.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#include <mbedtls/sha256.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#include <sys/acl.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#endif
#endif

namespace pulp::project_package::detail {

namespace {

#if defined(_WIN32)
using NtCreateFileFunction = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                              PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                              ULONG, PVOID, ULONG);

constexpr ULONG object_dont_reparse = 0x00001000L;

NtCreateFileFunction nt_create_file() noexcept {
    static const auto function = reinterpret_cast<NtCreateFileFunction>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    return function;
}

HANDLE open_relative(HANDLE parent, const std::filesystem::path& component, bool directory,
                     bool* created = nullptr) noexcept {
    if (created != nullptr)
        *created = false;
    const auto& name = component.native();
    if (name.empty() || name.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
        return INVALID_HANDLE_VALUE;

    UNICODE_STRING unicode_name{};
    unicode_name.Buffer = const_cast<PWSTR>(name.data());
    unicode_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode_name.MaximumLength = unicode_name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &unicode_name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE | object_dont_reparse;

    IO_STATUS_BLOCK status_block{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto access = directory ? FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
                                        FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE
                                  : GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
    const auto share =
        directory ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : FILE_SHARE_READ;
    const auto disposition = directory ? FILE_OPEN_IF : FILE_CREATE;
    const auto options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                         (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
                         (directory ? 0u : FILE_WRITE_THROUGH);
    const auto function = nt_create_file();
    if (function == nullptr ||
        function(&handle, access, &attributes, &status_block, nullptr, FILE_ATTRIBUTE_NORMAL, share,
                 disposition, options, nullptr, 0) < 0)
        return INVALID_HANDLE_VALUE;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = ::GetFileInformationByHandle(handle, &info) != 0 &&
                       (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                       ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
    if (!valid) {
        ::CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    if (created != nullptr)
        *created = status_block.Information == FILE_CREATED;
    return handle;
}

HANDLE open_relative_existing(HANDLE parent, const std::filesystem::path& component, bool directory,
                              ACCESS_MASK access, ULONG share) noexcept {
    const auto& name = component.native();
    if (name.empty() || name == L"." || name == L".." ||
        name.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
        return INVALID_HANDLE_VALUE;
    UNICODE_STRING unicode_name{};
    unicode_name.Buffer = const_cast<PWSTR>(name.data());
    unicode_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode_name.MaximumLength = unicode_name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &unicode_name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE | object_dont_reparse;
    IO_STATUS_BLOCK status_block{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                         (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    const auto function = nt_create_file();
    if (function == nullptr ||
        function(&handle, access | SYNCHRONIZE, &attributes, &status_block, nullptr,
                 FILE_ATTRIBUTE_NORMAL, share, FILE_OPEN, options, nullptr, 0) < 0)
        return INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = ::GetFileInformationByHandle(handle, &info) != 0 &&
                       (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                       ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
    if (!valid) {
        ::CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

HANDLE acquire_relative_lock(HANDLE parent, const std::filesystem::path& component) noexcept {
    const auto& name = component.native();
    if (name.empty() || name == L"." || name == L".." || !component.parent_path().empty() ||
        name.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
        return INVALID_HANDLE_VALUE;
    UNICODE_STRING unicode_name{};
    unicode_name.Buffer = const_cast<PWSTR>(name.data());
    unicode_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode_name.MaximumLength = unicode_name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &unicode_name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE | object_dont_reparse;
    IO_STATUS_BLOCK status_block{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto options =
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_NON_DIRECTORY_FILE;
    const auto function = nt_create_file();
    if (function == nullptr ||
        function(&handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attributes, &status_block,
                 nullptr, FILE_ATTRIBUTE_HIDDEN, 0, FILE_OPEN_IF, options, nullptr, 0) < 0)
        return INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid =
        ::GetFileInformationByHandle(handle, &info) != 0 &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        info.nNumberOfLinks == 1;
    if (!valid) {
        ::CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

std::optional<std::filesystem::path> final_path(HANDLE handle) noexcept {
    const auto count = ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (count == 0)
        return std::nullopt;
    std::wstring storage(count, L'\0');
    const auto written =
        ::GetFinalPathNameByHandleW(handle, storage.data(), count, FILE_NAME_NORMALIZED);
    if (written == 0 || written >= count)
        return std::nullopt;
    storage.resize(written);
    return std::filesystem::path(std::move(storage));
}

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

bool adopt_inherited_permissions(HANDLE child, HANDLE parent, bool directory) noexcept {
    PSECURITY_DESCRIPTOR parent_descriptor = nullptr;
    const auto read_error = GetSecurityInfo(
        parent, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &parent_descriptor);
    if (read_error != ERROR_SUCCESS || parent_descriptor == nullptr) {
        if (parent_descriptor != nullptr)
            LocalFree(parent_descriptor);
        return false;
    }
    HANDLE token = nullptr;
    bool token_opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != 0;
    if (!token_opened && GetLastError() == ERROR_NO_TOKEN)
        token_opened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0;
    GENERIC_MAPPING file_mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                                 FILE_ALL_ACCESS};
    PSECURITY_DESCRIPTOR child_descriptor = nullptr;
    const bool descriptor_created =
        token_opened &&
        CreatePrivateObjectSecurityEx(parent_descriptor, nullptr, &child_descriptor, nullptr,
                                      directory, SEF_DACL_AUTO_INHERIT, token, &file_mapping) != 0;
    if (token != nullptr)
        CloseHandle(token);
    LocalFree(parent_descriptor);
    if (!descriptor_created || child_descriptor == nullptr) {
        if (child_descriptor != nullptr)
            DestroyPrivateObjectSecurity(&child_descriptor);
        return false;
    }
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    PACL dacl = nullptr;
    const bool dacl_read =
        GetSecurityDescriptorDacl(child_descriptor, &dacl_present, &dacl, &dacl_defaulted) != 0 &&
        dacl_present && dacl != nullptr;
    const auto write_error =
        dacl_read
            ? SetSecurityInfo(child, SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, dacl, nullptr)
            : ERROR_INVALID_SECURITY_DESCR;
    DestroyPrivateObjectSecurity(&child_descriptor);
    return write_error == ERROR_SUCCESS;
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

int open_unlinked_permission_probe(int parent) noexcept {
    static std::atomic<std::uint64_t> serial{0};
    for (std::size_t attempt = 0; attempt < 32; ++attempt) {
        std::array<char, 96> name{};
        const auto value = serial.fetch_add(1, std::memory_order_relaxed);
        const auto length = std::snprintf(
            name.data(), name.size(), ".pulp-stage-permission-probe-%llu-%llu",
            static_cast<unsigned long long>(::getpid()), static_cast<unsigned long long>(value));
        if (length <= 0 || static_cast<std::size_t>(length) >= name.size())
            return -1;
        const auto descriptor = ::openat(parent, name.data(),
                                         O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC
#ifdef O_NOFOLLOW
                                             | O_NOFOLLOW
#endif
                                         ,
                                         0666);
        if (descriptor < 0) {
            if (errno == EEXIST)
                continue;
            return -1;
        }
        if (::unlinkat(parent, name.data(), 0) == 0)
            return descriptor;
        const auto saved_error = errno;
        ::close(descriptor);
        ::unlinkat(parent, name.data(), 0);
        errno = saved_error;
        return -1;
    }
    errno = EEXIST;
    return -1;
}

bool adopt_direct_child_permissions(int child, int parent) noexcept {
    const auto probe = open_unlinked_permission_probe(parent);
    if (probe < 0)
        return false;
    struct stat probe_status{};
    struct stat child_status{};
    bool valid = ::fstat(probe, &probe_status) == 0 && ::fstat(child, &child_status) == 0;

#if defined(__APPLE__)
    errno = 0;
    acl_t probe_acl = valid ? ::acl_get_fd_np(probe, ACL_TYPE_EXTENDED) : nullptr;
    const bool acl_read = probe_acl != nullptr || errno == ENOENT;
#elif defined(__linux__)
    constexpr const char* access_acl = "system.posix_acl_access";
    errno = 0;
    const auto acl_size = valid ? ::fgetxattr(probe, access_acl, nullptr, 0) : -1;
    const bool acl_supported = acl_size >= 0 || errno != ENOTSUP;
    const bool acl_read = acl_size >= 0 || errno == ENODATA || errno == ENOTSUP;
    std::vector<std::uint8_t> probe_acl;
    if (acl_size > 0) {
        probe_acl.resize(static_cast<std::size_t>(acl_size));
        valid = ::fgetxattr(probe, access_acl, probe_acl.data(), probe_acl.size()) == acl_size;
    }
#else
    constexpr bool acl_read = true;
#endif
    const bool probe_closed = ::close(probe) == 0;
    valid = valid && acl_read && probe_closed;
    if (!valid) {
#if defined(__APPLE__)
        if (probe_acl != nullptr)
            ::acl_free(probe_acl);
#endif
        return false;
    }
    if (child_status.st_uid != probe_status.st_uid)
        valid = false;
    if (valid && child_status.st_gid != probe_status.st_gid &&
        ::fchown(child, static_cast<uid_t>(-1), probe_status.st_gid) != 0)
        valid = false;
    if (valid && ::fchmod(child, probe_status.st_mode & 07777) != 0)
        valid = false;

#if defined(__APPLE__)
    if (valid && probe_acl != nullptr)
        valid = ::acl_set_fd_np(child, probe_acl, ACL_TYPE_EXTENDED) == 0;
    if (valid && probe_acl == nullptr) {
        acl_t empty = ::acl_init(0);
        errno = 0;
        valid = empty != nullptr &&
                (::acl_set_fd_np(child, empty, ACL_TYPE_EXTENDED) == 0 || errno == ENOENT);
        if (empty != nullptr)
            ::acl_free(empty);
    }
    if (probe_acl != nullptr)
        ::acl_free(probe_acl);
#elif defined(__linux__)
    if (valid && acl_size >= 0)
        valid = ::fsetxattr(child, access_acl, probe_acl.data(), probe_acl.size(), 0) == 0;
    if (valid && acl_size < 0 && acl_supported) {
        errno = 0;
        valid = ::fremovexattr(child, access_acl) == 0 || errno == ENODATA;
    }
#endif
    return valid;
}
#endif

} // namespace

AnchoredDirectory::~AnchoredDirectory() {
    close();
}

AnchoredDirectory::AnchoredDirectory(AnchoredDirectory&& other) noexcept
    : native_(std::exchange(other.native_, -1)) {}

AnchoredDirectory& AnchoredDirectory::operator=(AnchoredDirectory&& other) noexcept {
    if (this != &other) {
        close();
        native_ = std::exchange(other.native_, -1);
    }
    return *this;
}

PinnedFile::~PinnedFile() {
    close();
}

PinnedFile::PinnedFile(PinnedFile&& other) noexcept : native_(std::exchange(other.native_, -1)) {}

PinnedFile& PinnedFile::operator=(PinnedFile&& other) noexcept {
    if (this != &other) {
        close();
        native_ = std::exchange(other.native_, -1);
    }
    return *this;
}

std::optional<PinnedFile> PinnedFile::open(const std::filesystem::path& path, bool fence_capable,
                                           bool allow_rename, bool permissions_mutable) noexcept {
#if defined(_WIN32)
    auto access = fence_capable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    if (permissions_mutable)
        access |= WRITE_DAC;
    const auto sharing = FILE_SHARE_READ | (allow_rename ? FILE_SHARE_DELETE : 0);
    const auto handle =
        ::CreateFileW(path.c_str(), access, sharing, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks != 1) {
        ::CloseHandle(handle);
        return std::nullopt;
    }
    return PinnedFile(reinterpret_cast<std::intptr_t>(handle));
#else
    (void)allow_rename;
    (void)permissions_mutable;
    const auto descriptor = ::open(path.c_str(), (fence_capable ? O_RDWR : O_RDONLY) | O_CLOEXEC
#ifdef O_NOFOLLOW
                                                     | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return std::nullopt;
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
        ::close(descriptor);
        return std::nullopt;
    }
    return PinnedFile(descriptor);
#endif
}

std::optional<PinnedFile> PinnedFile::open(const AnchoredDirectory& parent,
                                           const std::filesystem::path& relative,
                                           bool fence_capable, bool allow_rename) noexcept {
    if (parent.native_ == -1 || relative.empty() || relative.is_absolute() ||
        !relative.parent_path().empty() || relative == "." || relative == "..")
        return std::nullopt;
#if defined(_WIN32)
    const auto access = fence_capable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    const auto sharing = FILE_SHARE_READ | (allow_rename ? FILE_SHARE_DELETE : 0);
    const auto handle = open_relative_existing(reinterpret_cast<HANDLE>(parent.native_), relative,
                                               false, access, sharing);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0 || info.nNumberOfLinks > 1) {
        ::CloseHandle(handle);
        return std::nullopt;
    }
    return PinnedFile(reinterpret_cast<std::intptr_t>(handle));
#else
    (void)allow_rename;
    const auto descriptor = ::openat(static_cast<int>(parent.native_), relative.c_str(),
                                     (fence_capable ? O_RDWR : O_RDONLY) | O_CLOEXEC
#ifdef O_NOFOLLOW
                                         | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return std::nullopt;
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink > 1) {
        ::close(descriptor);
        return std::nullopt;
    }
    return PinnedFile(descriptor);
#endif
}

std::optional<PinnedFile> PinnedFile::acquire_lock(const AnchoredDirectory& parent,
                                                   const std::filesystem::path& relative) noexcept {
    if (parent.native_ == -1 || relative.empty() || relative.is_absolute() ||
        !relative.parent_path().empty() || relative == "." || relative == "..")
        return std::nullopt;
#if defined(_WIN32)
    const auto handle = acquire_relative_lock(reinterpret_cast<HANDLE>(parent.native_), relative);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    return PinnedFile(reinterpret_cast<std::intptr_t>(handle));
#else
    const auto descriptor = ::openat(static_cast<int>(parent.native_), relative.c_str(),
                                     O_CREAT | O_RDWR | O_CLOEXEC
#ifdef O_NOFOLLOW
                                         | O_NOFOLLOW
#endif
                                     ,
                                     0600);
    if (descriptor < 0)
        return std::nullopt;
    struct stat status{};
    const bool valid = ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
                       status.st_nlink == 1 && ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
    if (!valid) {
        ::close(descriptor);
        return std::nullopt;
    }
    return PinnedFile(descriptor);
#endif
}

NativeReadOutcome PinnedFile::read_bounded(std::uint64_t maximum_bytes,
                                           std::vector<std::uint8_t>& bytes) const noexcept {
    bytes.clear();
    if (native_ == -1)
        return NativeReadOutcome::IoError;
#if defined(_WIN32)
    const auto handle = reinterpret_cast<HANDLE>(native_);
    BY_HANDLE_FILE_INFORMATION before{};
    if (::GetFileInformationByHandle(handle, &before) == 0 ||
        (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0 ||
        before.nNumberOfLinks > 1)
        return NativeReadOutcome::InvalidFile;
    const auto size =
        (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32u) | before.nFileSizeLow;
    if (size > maximum_bytes || size > static_cast<std::uint64_t>(SIZE_MAX))
        return NativeReadOutcome::LimitExceeded;
    LARGE_INTEGER beginning{};
    if (::SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == 0)
        return NativeReadOutcome::IoError;
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<std::size_t>(0x7ffff000u)));
        DWORD count = 0;
        if (!::ReadFile(handle, bytes.data() + offset, request, &count, nullptr) || count == 0) {
            bytes.clear();
            return NativeReadOutcome::IoError;
        }
        offset += count;
    }
    std::uint8_t trailing = 0;
    DWORD trailing_count = 0;
    BY_HANDLE_FILE_INFORMATION after{};
    if (::ReadFile(handle, &trailing, 1, &trailing_count, nullptr) == 0 || trailing_count != 0 ||
        ::GetFileInformationByHandle(handle, &after) == 0 || after.nNumberOfLinks > 1 ||
        before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
        before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime ||
        before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime) {
        bytes.clear();
        return NativeReadOutcome::InvalidFile;
    }
#else
    const auto descriptor = static_cast<int>(native_);
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink > 1)
        return NativeReadOutcome::InvalidFile;
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > maximum_bytes ||
        static_cast<std::uint64_t>(before.st_size) > static_cast<std::uint64_t>(SIZE_MAX))
        return NativeReadOutcome::LimitExceeded;
    if (::lseek(descriptor, 0, SEEK_SET) < 0)
        return NativeReadOutcome::IoError;
    bytes.resize(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            bytes.clear();
            return NativeReadOutcome::IoError;
        }
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t trailing = 0;
    const auto trailing_count = ::read(descriptor, &trailing, 1);
    struct stat after{};
    const bool stat_after = ::fstat(descriptor, &after) == 0;
#if defined(__APPLE__)
    const bool timestamps_match = stat_after &&
                                  before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
                                  before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
#else
    const bool timestamps_match = stat_after && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                                  before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
#endif
    if (trailing_count != 0 || !stat_after || after.st_nlink > 1 ||
        before.st_size != after.st_size || !timestamps_match) {
        bytes.clear();
        return NativeReadOutcome::InvalidFile;
    }
#endif
    return NativeReadOutcome::Ok;
}

bool PinnedFile::hash_matches(std::string_view expected_hex,
                              std::uint64_t maximum_bytes) const noexcept {
    if (native_ == -1)
        return false;
    std::uint64_t size = 0;
#if defined(_WIN32)
    const auto handle = reinterpret_cast<HANDLE>(native_);
    BY_HANDLE_FILE_INFORMATION before{};
    if (::GetFileInformationByHandle(handle, &before) == 0 ||
        (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0 ||
        before.nNumberOfLinks != 1)
        return false;
    size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32u) | before.nFileSizeLow;
    if (size > maximum_bytes)
        return false;
    LARGE_INTEGER beginning{};
    if (::SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == 0)
        return false;
#else
    const auto descriptor = static_cast<int>(native_);
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1)
        return false;
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > maximum_bytes)
        return false;
    size = static_cast<std::uint64_t>(before.st_size);
    if (::lseek(descriptor, 0, SEEK_SET) < 0)
        return false;
#endif

    invoke_fault_hook(PackageFaultPoint::BlobHashSnapshot);

    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts(&hash, 0);
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t offset = 0;
    while (offset < size) {
        const auto request = static_cast<std::size_t>(
            (std::min)(size - offset, static_cast<std::uint64_t>(buffer.size())));
#if defined(_WIN32)
        DWORD count = 0;
        if (!::ReadFile(handle, buffer.data(), static_cast<DWORD>(request), &count, nullptr) ||
            count == 0) {
            mbedtls_sha256_free(&hash);
            return false;
        }
#else
        auto count = ::read(descriptor, buffer.data(), request);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            mbedtls_sha256_free(&hash);
            return false;
        }
#endif
        mbedtls_sha256_update(&hash, buffer.data(), static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }

#if defined(_WIN32)
    std::uint8_t trailing = 0;
    DWORD trailing_count = 0;
    BY_HANDLE_FILE_INFORMATION after{};
    const bool stable =
        ::ReadFile(handle, &trailing, 1, &trailing_count, nullptr) != 0 && trailing_count == 0 &&
        ::GetFileInformationByHandle(handle, &after) != 0 &&
        before.nFileSizeHigh == after.nFileSizeHigh && before.nFileSizeLow == after.nFileSizeLow &&
        before.ftLastWriteTime.dwHighDateTime == after.ftLastWriteTime.dwHighDateTime &&
        before.ftLastWriteTime.dwLowDateTime == after.ftLastWriteTime.dwLowDateTime &&
        before.nNumberOfLinks == after.nNumberOfLinks;
#else
    std::uint8_t trailing = 0;
    const auto trailing_count = ::read(descriptor, &trailing, 1);
    struct stat after{};
    const bool stat_after = ::fstat(descriptor, &after) == 0;
#if defined(__APPLE__)
    const bool timestamps_match = stat_after &&
                                  before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
                                  before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
                                  before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
                                  before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    const bool timestamps_match = stat_after && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                                  before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
                                  before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
                                  before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
    const bool stable = trailing_count == 0 && stat_after && before.st_size == after.st_size &&
                        before.st_nlink == after.st_nlink && timestamps_match;
#endif
    if (!stable) {
        mbedtls_sha256_free(&hash);
        return false;
    }
    std::array<std::uint8_t, 32> digest{};
    mbedtls_sha256_finish(&hash, digest.data());
    mbedtls_sha256_free(&hash);
    static constexpr char digits[] = "0123456789abcdef";
    std::array<char, 64> encoded{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        encoded[index * 2] = digits[digest[index] >> 4];
        encoded[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    return expected_hex == std::string_view(encoded.data(), encoded.size());
}

bool PinnedFile::fence() const noexcept {
    if (native_ == -1)
        return false;
#if defined(_WIN32)
    return ::FlushFileBuffers(reinterpret_cast<HANDLE>(native_)) != 0;
#else
    return fence_descriptor(static_cast<int>(native_));
#endif
}

bool PinnedFile::adopt_inherited_permissions_from(const AnchoredDirectory& parent) const noexcept {
#if defined(_WIN32)
    return native_ != -1 && parent.native_ != -1 &&
           adopt_inherited_permissions(reinterpret_cast<HANDLE>(native_),
                                       reinterpret_cast<HANDLE>(parent.native_), false);
#else
    return native_ != -1 && parent.native_ != -1 &&
           adopt_direct_child_permissions(static_cast<int>(native_),
                                          static_cast<int>(parent.native_));
#endif
}

bool PinnedFile::still_named_by(const std::filesystem::path& path) const noexcept {
    if (native_ == -1)
        return false;
#if defined(_WIN32)
    const auto named = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (named == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION pinned_info{};
    BY_HANDLE_FILE_INFORMATION named_info{};
    const bool matches =
        ::GetFileInformationByHandle(reinterpret_cast<HANDLE>(native_), &pinned_info) != 0 &&
        ::GetFileInformationByHandle(named, &named_info) != 0 && pinned_info.nNumberOfLinks == 1 &&
        named_info.nNumberOfLinks == 1 &&
        (named_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
            0 &&
        pinned_info.dwVolumeSerialNumber == named_info.dwVolumeSerialNumber &&
        pinned_info.nFileIndexHigh == named_info.nFileIndexHigh &&
        pinned_info.nFileIndexLow == named_info.nFileIndexLow;
    ::CloseHandle(named);
    return matches;
#else
    struct stat pinned_status{};
    struct stat named_status{};
    return ::fstat(static_cast<int>(native_), &pinned_status) == 0 && pinned_status.st_nlink == 1 &&
           ::lstat(path.c_str(), &named_status) == 0 && S_ISREG(named_status.st_mode) &&
           named_status.st_nlink == 1 && pinned_status.st_dev == named_status.st_dev &&
           pinned_status.st_ino == named_status.st_ino;
#endif
}

bool PinnedFile::still_named_by(const AnchoredDirectory& parent,
                                const std::filesystem::path& relative) const noexcept {
    if (native_ == -1 || parent.native_ == -1 || relative.empty() || relative.is_absolute() ||
        !relative.parent_path().empty() || relative == "." || relative == "..")
        return false;
#if defined(_WIN32)
    const auto named = open_relative_existing(
        reinterpret_cast<HANDLE>(parent.native_), relative, false, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (named == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION pinned_info{};
    BY_HANDLE_FILE_INFORMATION named_info{};
    const bool matches =
        ::GetFileInformationByHandle(reinterpret_cast<HANDLE>(native_), &pinned_info) != 0 &&
        ::GetFileInformationByHandle(named, &named_info) != 0 && pinned_info.nNumberOfLinks == 1 &&
        named_info.nNumberOfLinks == 1 &&
        pinned_info.dwVolumeSerialNumber == named_info.dwVolumeSerialNumber &&
        pinned_info.nFileIndexHigh == named_info.nFileIndexHigh &&
        pinned_info.nFileIndexLow == named_info.nFileIndexLow;
    ::CloseHandle(named);
    return matches;
#else
    struct stat pinned_status{};
    struct stat named_status{};
    return ::fstat(static_cast<int>(native_), &pinned_status) == 0 && pinned_status.st_nlink == 1 &&
           ::fstatat(static_cast<int>(parent.native_), relative.c_str(), &named_status,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(named_status.st_mode) && named_status.st_nlink == 1 &&
           pinned_status.st_dev == named_status.st_dev &&
           pinned_status.st_ino == named_status.st_ino;
#endif
}

void PinnedFile::close() noexcept {
    if (native_ == -1)
        return;
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(native_));
#else
    ::close(static_cast<int>(native_));
#endif
    native_ = -1;
}

std::optional<AnchoredDirectory> AnchoredDirectory::open(const std::filesystem::path& path,
                                                         bool allow_rename) noexcept {
#if defined(_WIN32)
    const auto sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE | (allow_rename ? FILE_SHARE_DELETE : 0);
    const auto handle =
        ::CreateFileW(path.c_str(),
                      FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE |
                          FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
                      sharing, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        ::CloseHandle(handle);
        return std::nullopt;
    }
    return AnchoredDirectory(reinterpret_cast<std::intptr_t>(handle));
#else
    (void)allow_rename;
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                     | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                                     | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return std::nullopt;
    return AnchoredDirectory(descriptor);
#endif
}

std::optional<AnchoredDirectory>
AnchoredDirectory::open_directory(const std::filesystem::path& relative, bool allow_rename,
                                  bool permissions_mutable, bool writable) const noexcept {
    if (native_ == -1 || relative.empty() || relative.is_absolute() ||
        !relative.parent_path().empty() || relative == "." || relative == "..")
        return std::nullopt;
#if defined(_WIN32)
    const auto sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE | (allow_rename ? FILE_SHARE_DELETE : 0);
    auto access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL;
    if (writable)
        access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    if (permissions_mutable)
        access |= WRITE_DAC;
    const auto handle =
        open_relative_existing(reinterpret_cast<HANDLE>(native_), relative, true, access, sharing);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    return AnchoredDirectory(reinterpret_cast<std::intptr_t>(handle));
#else
    (void)allow_rename;
    (void)permissions_mutable;
    (void)writable;
    const auto descriptor = ::openat(static_cast<int>(native_), relative.c_str(),
                                     O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                         | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                         | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return std::nullopt;
    return AnchoredDirectory(descriptor);
#endif
}

std::optional<AnchoredDirectory>
AnchoredDirectory::open_or_create_directory(const std::filesystem::path& relative,
                                            bool& created) const noexcept {
    created = false;
    if (native_ == -1 || relative.empty() || relative.is_absolute() ||
        !relative.parent_path().empty() || relative == "." || relative == "..")
        return std::nullopt;
#if defined(_WIN32)
    const auto handle = open_relative(reinterpret_cast<HANDLE>(native_), relative, true, &created);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    return AnchoredDirectory(reinterpret_cast<std::intptr_t>(handle));
#else
    if (::mkdirat(static_cast<int>(native_), relative.c_str(), 0777) == 0) {
        created = true;
    } else if (errno != EEXIST) {
        return std::nullopt;
    }
    const auto descriptor = ::openat(static_cast<int>(native_), relative.c_str(),
                                     O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                         | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                         | O_NOFOLLOW
#endif
    );
    if (descriptor < 0)
        return std::nullopt;
    return AnchoredDirectory(descriptor);
#endif
}

bool AnchoredDirectory::still_named_by(const std::filesystem::path& path) const noexcept {
    if (native_ == -1)
        return false;
#if defined(_WIN32)
    const auto named = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (named == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION pinned_info{};
    BY_HANDLE_FILE_INFORMATION named_info{};
    const bool matches =
        ::GetFileInformationByHandle(reinterpret_cast<HANDLE>(native_), &pinned_info) != 0 &&
        ::GetFileInformationByHandle(named, &named_info) != 0 &&
        (named_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (named_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        pinned_info.dwVolumeSerialNumber == named_info.dwVolumeSerialNumber &&
        pinned_info.nFileIndexHigh == named_info.nFileIndexHigh &&
        pinned_info.nFileIndexLow == named_info.nFileIndexLow;
    ::CloseHandle(named);
    return matches;
#else
    struct stat pinned_status{};
    struct stat named_status{};
    return ::fstat(static_cast<int>(native_), &pinned_status) == 0 &&
           ::lstat(path.c_str(), &named_status) == 0 && S_ISDIR(named_status.st_mode) &&
           pinned_status.st_dev == named_status.st_dev &&
           pinned_status.st_ino == named_status.st_ino;
#endif
}

bool AnchoredDirectory::write_exclusive_and_fence(const std::filesystem::path& relative,
                                                  std::span<const std::uint8_t> bytes,
                                                  PackageFaultPoint written_point,
                                                  PackageFaultPoint fenced_point) const noexcept {
    if (native_ == -1 || relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory())
        return false;
    std::vector<std::filesystem::path> components;
    for (const auto& component : relative) {
        if (component.empty() || component == "." || component == "..")
            return false;
        components.push_back(component);
    }
    if (components.empty())
        return false;

#if defined(_WIN32)
    const auto root = reinterpret_cast<HANDLE>(native_);
    auto current = root;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const auto next = open_relative(current, components[index], true);
        if (current != root)
            ::CloseHandle(current);
        if (next == INVALID_HANDLE_VALUE)
            return false;
        current = next;
    }
    const auto file = open_relative(current, components.back(), false);
    if (current != root)
        ::CloseHandle(current);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool written = write_all(file, bytes);
    if (written)
        invoke_fault_hook(written_point);
    const bool fenced = written && ::FlushFileBuffers(file) != 0;
    if (fenced)
        invoke_fault_hook(fenced_point);
    const bool closed = ::CloseHandle(file) != 0;
    return fenced && closed;
#else
    const auto root = static_cast<int>(native_);
    auto current = root;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        if (::mkdirat(current, components[index].c_str(), 0777) != 0 && errno != EEXIST) {
            if (current != root)
                ::close(current);
            return false;
        }
        const auto next = ::openat(current, components[index].c_str(),
                                   O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                       | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                       | O_NOFOLLOW
#endif
        );
        if (current != root)
            ::close(current);
        if (next < 0)
            return false;
        current = next;
    }
    const auto descriptor = ::openat(current, components.back().c_str(),
                                     O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                         | O_NOFOLLOW
#endif
                                     ,
                                     0666);
    if (current != root)
        ::close(current);
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

NoReplaceOutcome AnchoredDirectory::publish_no_replace(
    const std::filesystem::path& source_name, const AnchoredDirectory& destination_parent,
    const std::filesystem::path& destination_name, NoReplaceSourceKind kind) const noexcept {
    const auto direct_name = [](const std::filesystem::path& name) {
        return !name.empty() && !name.is_absolute() && name.parent_path().empty() && name != "." &&
               name != "..";
    };
    if (native_ == -1 || destination_parent.native_ == -1 || !direct_name(source_name) ||
        !direct_name(destination_name))
        return NoReplaceOutcome::Failed;
#if defined(_WIN32)
    const auto source_parent_path = final_path(reinterpret_cast<HANDLE>(native_));
    const auto destination_parent_path =
        final_path(reinterpret_cast<HANDLE>(destination_parent.native_));
    if (!source_parent_path || !destination_parent_path)
        return NoReplaceOutcome::Failed;
    const auto source = *source_parent_path / source_name;
    const auto destination = *destination_parent_path / destination_name;
    const bool published =
        ::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
    const auto error = published ? ERROR_SUCCESS : ::GetLastError();
    if (published)
        return NoReplaceOutcome::Published;
    return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
               ? NoReplaceOutcome::DestinationExists
               : NoReplaceOutcome::Failed;
#elif defined(__APPLE__)
    if (::renameatx_np(static_cast<int>(native_), source_name.c_str(),
                       static_cast<int>(destination_parent.native_), destination_name.c_str(),
                       RENAME_EXCL) == 0)
        return NoReplaceOutcome::Published;
    return errno == EEXIST ? NoReplaceOutcome::DestinationExists : NoReplaceOutcome::Failed;
#elif defined(__linux__)
    if (::syscall(SYS_renameat2, static_cast<int>(native_), source_name.c_str(),
                  static_cast<int>(destination_parent.native_), destination_name.c_str(),
                  RENAME_NOREPLACE) == 0)
        return NoReplaceOutcome::Published;
    if (errno == EEXIST)
        return NoReplaceOutcome::DestinationExists;
    if (errno != ENOSYS && errno != EINVAL)
        return NoReplaceOutcome::Failed;
    if (kind == NoReplaceSourceKind::Directory)
        return NoReplaceOutcome::Unsupported;
    if (::linkat(static_cast<int>(native_), source_name.c_str(),
                 static_cast<int>(destination_parent.native_), destination_name.c_str(), 0) != 0)
        return errno == EEXIST ? NoReplaceOutcome::DestinationExists : NoReplaceOutcome::Failed;
    return ::unlinkat(static_cast<int>(native_), source_name.c_str(), 0) == 0
               ? NoReplaceOutcome::Published
               : NoReplaceOutcome::PublishedSourceRetained;
#else
    (void)kind;
    return NoReplaceOutcome::Unsupported;
#endif
}

bool AnchoredDirectory::fence() const noexcept {
    if (native_ == -1)
        return false;
#if defined(_WIN32)
    // The anchored Windows publication operation is MOVEFILE_WRITE_THROUGH;
    // no additional directory-handle flush is available or required here.
    return true;
#else
    return ::fsync(static_cast<int>(native_)) == 0;
#endif
}

bool AnchoredDirectory::adopt_inherited_permissions_from(
    const AnchoredDirectory& parent) const noexcept {
#if defined(_WIN32)
    return native_ != -1 && parent.native_ != -1 &&
           adopt_inherited_permissions(reinterpret_cast<HANDLE>(native_),
                                       reinterpret_cast<HANDLE>(parent.native_), true);
#else
    (void)parent;
    return native_ != -1;
#endif
}

void AnchoredDirectory::close() noexcept {
    if (native_ == -1)
        return;
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(native_));
#else
    ::close(static_cast<int>(native_));
#endif
    native_ = -1;
}

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
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
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

NoReplaceOutcome publish_no_replace_fallback(const std::filesystem::path& source,
                                             const std::filesystem::path& destination,
                                             NoReplaceSourceKind kind) noexcept {
#if defined(_WIN32)
    (void)source;
    (void)destination;
    (void)kind;
    return NoReplaceOutcome::Unsupported;
#else
    if (kind == NoReplaceSourceKind::Directory)
        return NoReplaceOutcome::Unsupported;
    if (::link(source.c_str(), destination.c_str()) != 0)
        return errno == EEXIST ? NoReplaceOutcome::DestinationExists : NoReplaceOutcome::Failed;
    // The destination becomes visible atomically when link() succeeds. Failure
    // to remove the private source is cleanup debt, not a failed publication;
    // reporting failure here would let callers retry after publication.
    return ::unlink(source.c_str()) == 0 ? NoReplaceOutcome::Published
                                         : NoReplaceOutcome::PublishedSourceRetained;
#endif
}

NoReplaceOutcome publish_no_replace(const std::filesystem::path& source,
                                    const std::filesystem::path& destination,
                                    NoReplaceSourceKind kind) noexcept {
#if defined(_WIN32)
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
        return NoReplaceOutcome::Published;
    const auto error = ::GetLastError();
    return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
               ? NoReplaceOutcome::DestinationExists
               : NoReplaceOutcome::Failed;
#elif defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0)
        return NoReplaceOutcome::Published;
    return errno == EEXIST ? NoReplaceOutcome::DestinationExists : NoReplaceOutcome::Failed;
#elif defined(__linux__)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) == 0)
        return NoReplaceOutcome::Published;
    if (errno == EEXIST)
        return NoReplaceOutcome::DestinationExists;
    if (errno != ENOSYS && errno != EINVAL)
        return NoReplaceOutcome::Failed;
    // link/unlink is an atomic no-replace fallback for regular files only.
    // POSIX forbids hard-linking directories, and plain rename() could replace
    // an empty destination created by a racing publisher, so fail closed when
    // the kernel/filesystem lacks RENAME_NOREPLACE for a directory tree.
    return publish_no_replace_fallback(source, destination, kind);
#else
    (void)source;
    (void)destination;
    (void)kind;
    return NoReplaceOutcome::Unsupported;
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
    const auto handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
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
