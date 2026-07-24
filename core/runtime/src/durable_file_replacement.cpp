#include <pulp/runtime/detail/durable_file_replacement.hpp>

#include "durable_file_replacement_test_access.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
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
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#endif

namespace pulp::runtime::detail {
namespace {

std::atomic<std::uint64_t> g_temporary_serial{0};

struct ReplacementMetadata {
    bool exists = false;
#if defined(_WIN32)
    std::vector<std::uint8_t> security_descriptor;
    DWORD file_attributes = FILE_ATTRIBUTE_NORMAL;
    DWORD volume_serial = 0;
    DWORD file_index_high = 0;
    DWORD file_index_low = 0;
#else
    struct stat status{};
    bool acl_supported = true;
#if defined(__APPLE__)
    acl_t acl = nullptr;

    ~ReplacementMetadata() {
        if (acl != nullptr)
            ::acl_free(acl);
    }
#elif defined(__linux__)
    std::optional<std::vector<std::uint8_t>> acl;
#endif
#endif
};

bool capture_replacement_metadata(const std::filesystem::path& destination,
                                  ReplacementMetadata& metadata) noexcept {
#if defined(_WIN32)
    constexpr SECURITY_INFORMATION kSecurityInformation =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    const auto attributes = ::GetFileAttributesW(destination.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return false;
    const auto identity_handle =
        ::CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (identity_handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION identity{};
    const auto identified = ::GetFileInformationByHandle(identity_handle, &identity) != 0;
    ::CloseHandle(identity_handle);
    if (!identified)
        return false;
    if (identity.nNumberOfLinks != 1)
        return false;
    DWORD required = 0;
    ::GetFileSecurityW(destination.c_str(), kSecurityInformation, nullptr, 0, &required);
    if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return false;
    metadata.security_descriptor.resize(required);
    if (!::GetFileSecurityW(destination.c_str(), kSecurityInformation,
                            metadata.security_descriptor.data(), required, &required))
        return false;
    metadata.file_attributes = attributes;
    metadata.volume_serial = identity.dwVolumeSerialNumber;
    metadata.file_index_high = identity.nFileIndexHigh;
    metadata.file_index_low = identity.nFileIndexLow;
    metadata.exists = true;
    return true;
#else
    if (::lstat(destination.c_str(), &metadata.status) != 0) {
        if (errno == ENOENT)
            return true;
        return false;
    }
    if (!S_ISREG(metadata.status.st_mode) || metadata.status.st_nlink != 1)
        return false;
    metadata.exists = true;
#if defined(__APPLE__)
    metadata.acl = ::acl_get_file(destination.c_str(), ACL_TYPE_EXTENDED);
    if (metadata.acl == nullptr) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP) {
            metadata.acl_supported = false;
        } else if (errno != ENOENT && errno != ENOATTR) {
            return false;
        }
    }
    if (metadata.acl != nullptr) {
        acl_entry_t entry = nullptr;
        const auto entry_result = ::acl_get_entry(metadata.acl, ACL_FIRST_ENTRY, &entry);
        if (entry_result < 0)
            return false;
        if (entry_result == 0) {
            ::acl_free(metadata.acl);
            metadata.acl = nullptr;
        }
    }
#elif defined(__linux__)
    constexpr auto kAclName = "system.posix_acl_access";
    errno = 0;
    const auto acl_size = ::getxattr(destination.c_str(), kAclName, nullptr, 0);
    if (acl_size < 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP)
            metadata.acl_supported = false;
        else if (errno != ENODATA)
            return false;
    } else {
        metadata.acl.emplace(static_cast<std::size_t>(acl_size));
        const auto copied =
            ::getxattr(destination.c_str(), kAclName, metadata.acl->data(), metadata.acl->size());
        if (copied != acl_size)
            return false;
    }
#endif
    return true;
#endif
}

bool destination_matches_metadata(const std::filesystem::path& destination,
                                  const ReplacementMetadata& metadata) noexcept {
#if defined(_WIN32)
    const auto attributes = ::GetFileAttributesW(destination.c_str());
    if (!metadata.exists) {
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return false;
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return false;
    const auto handle = ::CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION identity{};
    const auto identified = ::GetFileInformationByHandle(handle, &identity) != 0;
    ::CloseHandle(handle);
    return identified && identity.nNumberOfLinks == 1 &&
           identity.dwVolumeSerialNumber == metadata.volume_serial &&
           identity.nFileIndexHigh == metadata.file_index_high &&
           identity.nFileIndexLow == metadata.file_index_low;
#else
    struct stat current{};
    if (::lstat(destination.c_str(), &current) != 0)
        return !metadata.exists && errno == ENOENT;
    return metadata.exists && S_ISREG(current.st_mode) && current.st_nlink == 1 &&
           current.st_dev == metadata.status.st_dev && current.st_ino == metadata.status.st_ino;
#endif
}

#if defined(__APPLE__)
bool clear_extended_acl(int descriptor) noexcept {
    acl_t empty = ::acl_init(0);
    errno = 0;
    const bool cleared =
        empty != nullptr && ::acl_set_fd_np(descriptor, empty, ACL_TYPE_EXTENDED) == 0;
    const bool unsupported = !cleared && errno == EOPNOTSUPP;
    if (empty != nullptr)
        ::acl_free(empty);
    return cleared || unsupported;
}
#endif

bool apply_replacement_metadata(int descriptor, const ReplacementMetadata& metadata) noexcept {
    if (!metadata.exists)
        return true;
#if defined(_WIN32)
    (void)descriptor;
    return true;
#else
    struct stat temporary_status{};
    if (::fstat(descriptor, &temporary_status) != 0)
        return false;
    if ((temporary_status.st_uid != metadata.status.st_uid ||
         temporary_status.st_gid != metadata.status.st_gid) &&
        ::fchown(descriptor, metadata.status.st_uid, metadata.status.st_gid) != 0)
        return false;
    if (::fchmod(descriptor, metadata.status.st_mode & static_cast<mode_t>(07777)) != 0)
        return false;
#if defined(__APPLE__)
    if (!metadata.acl_supported)
        return true;
    if (metadata.acl != nullptr)
        return ::acl_set_fd_np(descriptor, metadata.acl, ACL_TYPE_EXTENDED) == 0;
    return clear_extended_acl(descriptor);
#elif defined(__linux__)
    if (!metadata.acl_supported)
        return true;
    constexpr auto kAclName = "system.posix_acl_access";
    if (metadata.acl)
        return ::fsetxattr(descriptor, kAclName, metadata.acl->data(), metadata.acl->size(), 0) ==
               0;
    return ::fremovexattr(descriptor, kAclName) == 0 || errno == ENODATA || errno == ENOTSUP ||
           errno == EOPNOTSUPP;
#else
    return true;
#endif
#endif
}

std::filesystem::path temporary_sibling(const std::filesystem::path& destination) {
#if defined(_WIN32)
    const auto process = static_cast<std::uint64_t>(_getpid());
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    auto temporary = destination;
    temporary += ".tmp." + std::to_string(process) + "." +
                 std::to_string(g_temporary_serial.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

int create_exclusive(const std::filesystem::path& path,
                     const ReplacementMetadata& metadata) noexcept {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security_attributes{};
    SECURITY_ATTRIBUTES* security = nullptr;
    if (metadata.exists) {
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.lpSecurityDescriptor =
            const_cast<std::uint8_t*>(metadata.security_descriptor.data());
        security = &security_attributes;
    }
    const auto handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        errno = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? EEXIST : EIO;
        return -1;
    }
    const auto descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(handle),
                                            _O_RDWR | _O_BINARY | _O_NOINHERIT);
    if (descriptor < 0)
        ::CloseHandle(handle);
    return descriptor;
#else
    (void)metadata;
    return ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
#endif
}

bool descriptor_matches_path(int descriptor, const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION held{};
    const auto held_handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
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
    return queried && held.nNumberOfLinks == 1 && named.nNumberOfLinks == 1 &&
           held.dwVolumeSerialNumber == named.dwVolumeSerialNumber &&
           held.nFileIndexHigh == named.nFileIndexHigh && held.nFileIndexLow == named.nFileIndexLow;
#else
    struct stat held{};
    struct stat named{};
    return ::fstat(descriptor, &held) == 0 && S_ISREG(held.st_mode) && held.st_nlink == 1 &&
           ::lstat(path.c_str(), &named) == 0 && S_ISREG(named.st_mode) &&
           held.st_dev == named.st_dev && held.st_ino == named.st_ino;
#endif
}

bool sync_descriptor(int descriptor) noexcept {
#if defined(_WIN32)
    return _commit(descriptor) == 0;
#elif defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0)
        return true;
    if (errno != ENOTSUP)
        return false;
    return ::fsync(descriptor) == 0;
#else
    return ::fsync(descriptor) == 0;
#endif
}

bool close_descriptor(int descriptor) noexcept {
#if defined(_WIN32)
    return _close(descriptor) == 0;
#else
    return ::close(descriptor) == 0;
#endif
}

bool replace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                  const ReplacementMetadata& metadata) noexcept {
#if defined(_WIN32)
    constexpr DWORD kSettableAttributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
                                          FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                                          FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_READONLY |
                                          FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY;
    auto replacement_attributes =
        metadata.exists ? metadata.file_attributes & kSettableAttributes : FILE_ATTRIBUTE_NORMAL;
    if (replacement_attributes == 0)
        replacement_attributes = FILE_ATTRIBUTE_NORMAL;
    if (::SetFileAttributesW(temporary.c_str(), replacement_attributes) == 0)
        return false;

    bool cleared_destination_read_only = false;
    if (metadata.exists && (metadata.file_attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        auto writable_attributes = metadata.file_attributes & ~FILE_ATTRIBUTE_READONLY;
        if (writable_attributes == 0)
            writable_attributes = FILE_ATTRIBUTE_NORMAL;
        if (::SetFileAttributesW(destination.c_str(), writable_attributes) == 0 ||
            !destination_matches_metadata(destination, metadata)) {
            ::SetFileAttributesW(temporary.c_str(), FILE_ATTRIBUTE_NORMAL);
            return false;
        }
        cleared_destination_read_only = true;
    }
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return true;
    if (cleared_destination_read_only && destination_matches_metadata(destination, metadata))
        ::SetFileAttributesW(destination.c_str(), metadata.file_attributes);
    ::SetFileAttributesW(temporary.c_str(), FILE_ATTRIBUTE_NORMAL);
    return false;
#else
    (void)metadata;
    return ::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}

bool sync_parent_directory(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    (void)path;
    return true;
#else
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    const auto descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
#endif
}

void remove_temporary(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

struct DurableFileReplacement::Impl {
    std::filesystem::path destination;
    std::filesystem::path temporary;
    ReplacementMetadata metadata;
    int descriptor = -1;
    bool fail_parent_directory_sync = false;
};

void DurableFileReplacementTestAccess::fail_parent_directory_sync(
    DurableFileReplacement& replacement) noexcept {
    if (replacement.impl_)
        replacement.impl_->fail_parent_directory_sync = true;
}

DurableFileReplacement::DurableFileReplacement(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

DurableFileReplacement::~DurableFileReplacement() {
    cancel();
}

DurableFileReplacement::DurableFileReplacement(DurableFileReplacement&&) noexcept = default;
DurableFileReplacement& DurableFileReplacement::operator=(DurableFileReplacement&& other) noexcept {
    if (this != &other) {
        cancel();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

std::optional<DurableFileReplacement>
DurableFileReplacement::create(const std::filesystem::path& destination) noexcept {
    try {
        auto impl = std::make_unique<Impl>();
        impl->destination = destination;
        if (!capture_replacement_metadata(destination, impl->metadata))
            return std::nullopt;
        for (std::size_t attempt = 0; attempt < 128; ++attempt) {
            impl->temporary = temporary_sibling(destination);
            impl->descriptor = create_exclusive(impl->temporary, impl->metadata);
            if (impl->descriptor >= 0)
                return DurableFileReplacement(std::move(impl));
            if (errno != EEXIST)
                break;
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

bool DurableFileReplacement::valid() const noexcept {
    return impl_ != nullptr && impl_->descriptor >= 0;
}

int DurableFileReplacement::native_descriptor() const noexcept {
    return valid() ? impl_->descriptor : -1;
}

const std::filesystem::path& DurableFileReplacement::temporary_path() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->temporary : empty;
}

bool DurableFileReplacement::write_all(std::span<const std::uint8_t> bytes) noexcept {
    if (!valid())
        return false;
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        const auto remaining = bytes.size() - consumed;
        const auto request = static_cast<unsigned int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(1) << 30));
#if defined(_WIN32)
        const auto count = _write(impl_->descriptor, bytes.data() + consumed, request);
#else
        const auto count = ::write(impl_->descriptor, bytes.data() + consumed, request);
#endif
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        consumed += static_cast<std::size_t>(count);
    }
    return true;
}

DurableFileCommitOutcome DurableFileReplacement::commit() noexcept {
    bool replaced = false;
    try {
        if (!valid())
            return DurableFileCommitOutcome::NotReplaced;
        if (!descriptor_matches_path(impl_->descriptor, impl_->temporary) ||
            !destination_matches_metadata(impl_->destination, impl_->metadata) ||
            !apply_replacement_metadata(impl_->descriptor, impl_->metadata) ||
            !sync_descriptor(impl_->descriptor)) {
            cancel();
            return DurableFileCommitOutcome::NotReplaced;
        }
        const auto descriptor = std::exchange(impl_->descriptor, -1);
        if (!close_descriptor(descriptor)) {
            cancel();
            return DurableFileCommitOutcome::NotReplaced;
        }
        if (!destination_matches_metadata(impl_->destination, impl_->metadata) ||
            !replace_file(impl_->temporary, impl_->destination, impl_->metadata)) {
            cancel();
            return DurableFileCommitOutcome::NotReplaced;
        }
        replaced = true;
        impl_->temporary.clear();
        if (impl_->fail_parent_directory_sync || !sync_parent_directory(impl_->destination))
            return DurableFileCommitOutcome::ReplacedButDirectorySyncFailed;
        return DurableFileCommitOutcome::ReplacedDurably;
    } catch (...) {
        if (replaced)
            return DurableFileCommitOutcome::ReplacedButDirectorySyncFailed;
        cancel();
        return DurableFileCommitOutcome::NotReplaced;
    }
}

void DurableFileReplacement::cancel() noexcept {
    if (!impl_)
        return;
    if (impl_->descriptor >= 0) {
        close_descriptor(impl_->descriptor);
        impl_->descriptor = -1;
    }
    if (!impl_->temporary.empty()) {
        remove_temporary(impl_->temporary);
        impl_->temporary.clear();
    }
}

} // namespace pulp::runtime::detail
