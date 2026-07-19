#include "sample_mip_sidecar_internal.hpp"

#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#else
#include <fcntl.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::audio::sample_mip_detail {

#ifndef _WIN32
bool parent_protects_private_child_entries(const std::filesystem::path& path) noexcept {
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0)
        return false;
    struct stat status{};
    const bool read = ::fstat(directory, &status) == 0;
    ::close(directory);
    if (!read || !S_ISDIR(status.st_mode))
        return false;
    const bool shared_writable = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    const bool trusted_sticky_owner = status.st_uid == ::geteuid() || status.st_uid == 0;
    return !shared_writable || ((status.st_mode & S_ISVTX) != 0 && trusted_sticky_owner);
}

bool make_directory_private(const std::filesystem::path& path) noexcept {
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0)
        return false;
    bool ok = ::fchmod(directory, 0700) == 0;
#if defined(__APPLE__)
    acl_t empty = ::acl_init(0);
    errno = 0;
    const bool acl_cleared =
        empty != nullptr && ::acl_set_fd_np(directory, empty, ACL_TYPE_EXTENDED) == 0;
    const bool acl_unsupported = !acl_cleared && errno == EOPNOTSUPP;
    ok = ok && (acl_cleared || acl_unsupported);
    if (empty != nullptr)
        ::acl_free(empty);
#elif defined(__linux__)
    for (const char* name : {"system.posix_acl_access", "system.posix_acl_default"}) {
        if (::fremovexattr(directory, name) != 0 && errno != ENODATA && errno != ENOTSUP)
            ok = false;
    }
#endif
    struct stat status{};
    ok = ok && ::fstat(directory, &status) == 0 && (status.st_mode & 0777) == 0700 &&
         status.st_uid == ::geteuid();
    ::close(directory);
    return ok;
}
#endif

std::filesystem::path unique_temporary_directory(const std::filesystem::path& parent) {
#ifndef _WIN32
    if (!parent_protects_private_child_entries(parent))
        return {};
#endif
    std::random_device random;
    for (int attempt = 0; attempt < 128; ++attempt) {
        const std::array<std::uint32_t, 4> nonce{random(), random(), random(), random()};
        const auto name =
            ".pulp-mip-tmp-" +
            runtime::hex_encode(reinterpret_cast<const std::uint8_t*>(nonce.data()), sizeof(nonce));
        const auto path = parent / name;
#ifdef _WIN32
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1,
                &descriptor, nullptr)) {
            return {};
        }
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        const bool created = CreateDirectoryW(path.c_str(), &attributes) != 0;
        LocalFree(descriptor);
        if (created)
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
#else
        if (::mkdir(path.c_str(), 0700) == 0) {
            if (make_directory_private(path))
                return path;
            std::error_code cleanup_error;
            std::filesystem::remove(path, cleanup_error);
            return {};
        }
        if (errno != EEXIST)
            return {};
#endif
    }
    return {};
}

runtime::FileIdentity temporary_directory_identity(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION information{};
    const bool read = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!read || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {};
    return {.volume = information.dwVolumeSerialNumber,
            .file = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                    information.nFileIndexLow,
            .generation = 0,
            .valid = true};
#else
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return {};
    return {.volume = static_cast<std::uint64_t>(status.st_dev),
            .file = static_cast<std::uint64_t>(status.st_ino),
            .generation = 0,
            .valid = true};
#endif
}

#ifdef _WIN32
bool temporary_directory_has_private_security(void* opaque_handle) noexcept {
    const auto handle = static_cast<HANDLE>(opaque_handle);
    PSECURITY_DESCRIPTOR expected_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1,
            &expected_descriptor, nullptr)) {
        return false;
    }
    PACL expected_dacl = nullptr;
    BOOL expected_present = FALSE;
    BOOL expected_defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(expected_descriptor, &expected_present, &expected_dacl,
                                   &expected_defaulted) ||
        !expected_present || expected_dacl == nullptr) {
        LocalFree(expected_descriptor);
        return false;
    }

    PSID owner = nullptr;
    PACL actual_dacl = nullptr;
    PSECURITY_DESCRIPTOR actual_descriptor = nullptr;
    const auto security_error = GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
        nullptr, &actual_dacl, nullptr, &actual_descriptor);
    HANDLE token = nullptr;
    DWORD token_bytes = 0;
    bool token_opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != 0;
    if (!token_opened && GetLastError() == ERROR_NO_TOKEN)
        token_opened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0;
    if (token_opened)
        GetTokenInformation(token, TokenOwner, nullptr, 0, &token_bytes);
    std::vector<std::uint8_t> token_storage(token_bytes);
    const bool token_read = token_opened && token_bytes != 0 &&
                            GetTokenInformation(token, TokenOwner, token_storage.data(),
                                                token_bytes, &token_bytes) != 0;
    if (token != nullptr)
        CloseHandle(token);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool control_read =
        actual_descriptor != nullptr &&
        GetSecurityDescriptorControl(actual_descriptor, &control, &revision) != 0;
    const auto* token_owner =
        token_read ? reinterpret_cast<const TOKEN_OWNER*>(token_storage.data()) : nullptr;
    const bool matches = security_error == ERROR_SUCCESS && owner != nullptr &&
                         token_owner != nullptr && EqualSid(owner, token_owner->Owner) != 0 &&
                         control_read && (control & SE_DACL_PROTECTED) != 0 &&
                         actual_dacl != nullptr && actual_dacl->AclSize == expected_dacl->AclSize &&
                         std::memcmp(actual_dacl, expected_dacl, expected_dacl->AclSize) == 0;
    if (actual_descriptor != nullptr)
        LocalFree(actual_descriptor);
    LocalFree(expected_descriptor);
    return matches;
}
#endif
PublishResult publish_no_replace(const std::filesystem::path& from,
                                 const std::filesystem::path& to) noexcept {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
        return PublishResult::Published;
    const auto error = GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
               ? PublishResult::AlreadyExists
               : PublishResult::Failed;
#else
    if (::link(from.c_str(), to.c_str()) == 0) {
        ::unlink(from.c_str());
        return PublishResult::Published;
    }
    return errno == EEXIST ? PublishResult::AlreadyExists : PublishResult::Failed;
#endif
}

bool replace_by_rename(const std::filesystem::path& from,
                       const std::filesystem::path& to) noexcept {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool sync_file_for_publication(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool synced = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return synced;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
#endif
}

bool sync_parent_directory(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    (void)path;
    return true;
#else
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
#endif
}

} // namespace pulp::audio::sample_mip_detail
