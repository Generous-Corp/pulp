#include "control_private_store.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::inspect::detail {

#ifdef _WIN32
namespace {

class OwnerOnlySecurity {
  public:
    OwnerOnlySecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        token_user_.resize(size);
        if (size == 0 || !GetTokenInformation(token, TokenUser, token_user_.data(), size, &size)) {
            CloseHandle(token);
            token_user_.clear();
            return;
        }
        CloseHandle(token);

        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = static_cast<wchar_t*>(user_sid());
        if (SetEntriesInAclW(1, &entry, nullptr, &acl_) != ERROR_SUCCESS ||
            !InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor_, user_sid(), FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            if (acl_)
                LocalFree(acl_);
            acl_ = nullptr;
            return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
        valid_ = true;
    }

    ~OwnerOnlySecurity() {
        if (acl_)
            LocalFree(acl_);
    }

    OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
    OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;

    bool valid() const {
        return valid_;
    }
    SECURITY_ATTRIBUTES* attributes() {
        return &attributes_;
    }
    PSID user_sid() const {
        return token_user_.empty()
                   ? nullptr
                   : reinterpret_cast<const TOKEN_USER*>(token_user_.data())->User.Sid;
    }

  private:
    std::vector<std::uint8_t> token_user_;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_ = false;
};

bool owner_private_handle(HANDLE handle, bool expect_directory, const OwnerOnlySecurity& expected) {
    BY_HANDLE_FILE_INFORMATION file_info{};
    if (!expected.valid() || !GetFileInformationByHandle(handle, &file_info) ||
        (file_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (((file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != expect_directory)) {
        return false;
    }

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        &owner, nullptr, &dacl, nullptr, &descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool control_read =
        descriptor && GetSecurityDescriptorControl(descriptor, &control, &revision);
    void* raw_ace = nullptr;
    const bool one_ace = dacl && dacl->AceCount == 1 && GetAce(dacl, 0, &raw_ace);
    const auto* header = one_ace ? static_cast<const ACE_HEADER*>(raw_ace) : nullptr;
    const auto* allowed = header && header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                                  (header->AceFlags & INHERITED_ACE) == 0
                              ? static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace)
                              : nullptr;
    ACCESS_MASK granted = allowed ? allowed->Mask : 0;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                            FILE_ALL_ACCESS};
    MapGenericMask(&granted, &mapping);
    const bool secure = status == ERROR_SUCCESS && owner && EqualSid(owner, expected.user_sid()) &&
                        control_read && (control & SE_DACL_PROTECTED) != 0 && allowed &&
                        EqualSid(const_cast<DWORD*>(&allowed->SidStart), expected.user_sid()) &&
                        (granted & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
    if (descriptor)
        LocalFree(descriptor);
    return secure;
}

bool owner_private_path(const std::filesystem::path& path, bool expect_directory) {
    OwnerOnlySecurity expected;
    if (!expected.valid())
        return false;
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
    if (expect_directory)
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    const HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool secure = owner_private_handle(handle, expect_directory, expected);
    CloseHandle(handle);
    return secure;
}

bool create_owner_private_directory_tree(const std::filesystem::path& directory) {
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return owner_private_path(directory, true);
    const auto attributes_error = GetLastError();
    if (attributes_error != ERROR_FILE_NOT_FOUND && attributes_error != ERROR_PATH_NOT_FOUND)
        return false;

    const auto parent = directory.parent_path();
    if (!parent.empty() && GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !create_owner_private_directory_tree(parent)) {
        return false;
    }

    OwnerOnlySecurity security;
    if (!security.valid())
        return false;
    if (!CreateDirectoryW(directory.c_str(), security.attributes()) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    return owner_private_path(directory, true);
}

} // namespace
#endif

bool ensure_owner_private_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    return !directory.empty() && create_owner_private_directory_tree(directory);
#else
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    struct stat status{};
    if (::lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != ::geteuid()) {
        return false;
    }
    if (::chmod(directory.c_str(), 0700) != 0 || ::lstat(directory.c_str(), &status) != 0) {
        return false;
    }
    return (status.st_mode & 077) == 0;
#endif
}

std::optional<std::vector<std::uint8_t>> read_owner_private_file(const std::filesystem::path& path,
                                                                 std::size_t maximum_bytes) {
#ifdef _WIN32
    OwnerOnlySecurity expected;
    if (!expected.valid())
        return std::nullopt;
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::nullopt;
    LARGE_INTEGER size{};
    if (!owner_private_handle(file, false, expected) || !GetFileSizeEx(file, &size) ||
        size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum_bytes) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto remaining = std::min<std::size_t>(
            contents.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!ReadFile(file, contents.data() + offset, static_cast<DWORD>(remaining), &count,
                      nullptr) ||
            count == 0) {
            CloseHandle(file);
            return std::nullopt;
        }
        offset += count;
    }
    CloseHandle(file);
    return contents;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0)
        return std::nullopt;
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & 077) != 0 || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
        ::close(descriptor);
        return std::nullopt;
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto count = ::read(descriptor, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(descriptor);
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0)
        return std::nullopt;
    return contents;
#endif
}

bool write_owner_private_file_atomic(const std::filesystem::path& destination,
                                     std::span<const std::uint8_t> contents) {
    const auto random = runtime::secure_random_bytes(8);
    if (!random)
        return false;
    auto temporary = destination;
    temporary += ".tmp-" + runtime::hex_encode(*random);
#ifdef _WIN32
    OwnerOnlySecurity security;
    if (!security.valid())
        return false;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL, 0,
                              security.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::size_t offset = 0;
    bool succeeded = owner_private_handle(file, false, security);
    while (succeeded && offset < contents.size()) {
        const auto remaining = std::min<std::size_t>(
            contents.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!WriteFile(file, contents.data() + offset, static_cast<DWORD>(remaining), &count,
                       nullptr) ||
            count == 0) {
            succeeded = false;
            break;
        }
        offset += count;
    }
    succeeded = succeeded && FlushFileBuffers(file);
    CloseHandle(file);
    if (!succeeded || !MoveFileExW(temporary.c_str(), destination.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!owner_private_path(destination, false)) {
        DeleteFileW(destination.c_str());
        return false;
    }
    return true;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(temporary.c_str(), flags, 0600);
    if (descriptor < 0)
        return false;
    bool succeeded = ::fchmod(descriptor, 0600) == 0;
    std::size_t offset = 0;
    while (succeeded && offset < contents.size()) {
        const auto count = ::write(descriptor, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            succeeded = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    succeeded = succeeded && ::fsync(descriptor) == 0;
    if (::close(descriptor) != 0)
        succeeded = false;
    if (!succeeded || ::rename(temporary.c_str(), destination.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    int directory_flags = O_RDONLY;
#ifdef O_CLOEXEC
    directory_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    directory_flags |= O_DIRECTORY;
#endif
    const int directory = ::open(destination.parent_path().c_str(), directory_flags);
    if (directory < 0)
        return false;
    const bool durable = ::fsync(directory) == 0;
    ::close(directory);
    return durable;
#endif
}

OwnerPrivateFilePublishResult publish_owner_private_file(const std::filesystem::path& destination,
                                                         std::span<const std::uint8_t> contents) {
    const auto random = runtime::secure_random_bytes(8);
    if (!random)
        return OwnerPrivateFilePublishResult::Failed;
    const auto temporary =
        destination.parent_path() / (".private-publish-" + runtime::hex_encode(*random));
    if (!write_owner_private_file_atomic(temporary, contents))
        return OwnerPrivateFilePublishResult::Failed;

    std::error_code error;
    std::filesystem::create_hard_link(temporary, destination, error);
    const bool existed = error == std::errc::file_exists;
    const bool published = !error;
    (void)remove_owner_private_file_durable(temporary);
    if (published
#ifdef _WIN32
        && owner_private_path(destination, false)
#endif
    )
        return OwnerPrivateFilePublishResult::Published;
    if (published)
        (void)remove_owner_private_file_durable(destination);
#ifdef _WIN32
    if (existed && !owner_private_path(destination, false))
        return OwnerPrivateFilePublishResult::Failed;
#endif
    return existed ? OwnerPrivateFilePublishResult::Exists : OwnerPrivateFilePublishResult::Failed;
}

bool remove_owner_private_file_durable(const std::filesystem::path& path) {
#ifdef _WIN32
    return DeleteFileW(path.c_str()) != 0;
#else
    if (::unlink(path.c_str()) != 0)
        return false;
    int directory_flags = O_RDONLY;
#ifdef O_CLOEXEC
    directory_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    directory_flags |= O_DIRECTORY;
#endif
    const int directory = ::open(path.parent_path().c_str(), directory_flags);
    if (directory < 0)
        return false;
    const bool durable = ::fsync(directory) == 0;
    ::close(directory);
    return durable;
#endif
}

} // namespace pulp::inspect::detail
