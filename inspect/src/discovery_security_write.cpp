#include "discovery_security.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <limits>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

namespace pulp::inspect::discovery_security {
namespace {
#ifdef _WIN32
class OwnerOnlySecurity {
public:
    OwnerOnlySecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        token_user_.resize(size);
        if (size == 0 ||
            !GetTokenInformation(token, TokenUser, token_user_.data(), size,
                                 &size)) {
            CloseHandle(token);
            token_user_.clear();
            return;
        }
        CloseHandle(token);

        auto* user = reinterpret_cast<TOKEN_USER*>(token_user_.data());
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName =
            static_cast<wchar_t*>(user->User.Sid);
        if (SetEntriesInAclW(1, &entry, nullptr, &acl_) != ERROR_SUCCESS ||
            !InitializeSecurityDescriptor(&descriptor_,
                                          SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                          SE_DACL_PROTECTED)) {
            if (acl_)
                LocalFree(acl_);
            acl_ = nullptr;
            return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        valid_ = true;
    }

    ~OwnerOnlySecurity() {
        if (acl_)
            LocalFree(acl_);
    }

    OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
    OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;

    bool valid() const { return valid_; }
    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
    PSID user_sid() const {
        if (token_user_.empty())
            return nullptr;
        return reinterpret_cast<const TOKEN_USER*>(token_user_.data())
            ->User.Sid;
    }

private:
    std::vector<std::uint8_t> token_user_;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_ = false;
};

bool create_owner_only_windows_directory_tree(
    const std::filesystem::path& directory) {
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return owner_private_path(directory, true);

    const auto parent = directory.parent_path();
    // Create every missing parent before the secured leaf. CreateDirectoryW
    // does not provide create_directories semantics, and using the filesystem
    // helper would lose the explicit owner-only security descriptor.
    if (!parent.empty() &&
        GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !create_owner_only_windows_directory_tree(parent)) {
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
#endif

#ifndef _WIN32
[[maybe_unused]] bool clear_extended_acl(int descriptor) {
#ifdef __APPLE__
    acl_t empty = ::acl_init(0);
    if (!empty)
        return false;
    const bool cleared =
        ::acl_set_fd_np(descriptor, empty, ACL_TYPE_EXTENDED) == 0;
    ::acl_free(empty);
    return cleared;
#else
    (void)descriptor;
    return true;
#endif
}

#endif

} // namespace

bool ensure_private_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    return create_owner_only_windows_directory_tree(directory);
#else
    std::error_code error;
    const auto parent = directory.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, error);
    bool created = false;
    if (error) {
        return false;
    }
    if (::mkdir(directory.c_str(), 0700) == 0)
        created = true;
    else if (errno != EEXIST)
        return false;

    struct stat before {};
    if (::lstat(directory.c_str(), &before) != 0 ||
        !S_ISDIR(before.st_mode) || S_ISLNK(before.st_mode) ||
        before.st_uid != ::geteuid()) {
        return false;
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(directory.c_str(), flags);
    if (descriptor < 0)
        return false;
    if (created &&
        (!clear_extended_acl(descriptor) ||
         ::fchmod(descriptor, 0700) != 0)) {
        ::close(descriptor);
        return false;
    }
    struct stat opened {};
    const bool same_directory =
        ::fstat(descriptor, &opened) == 0 &&
        S_ISDIR(opened.st_mode) &&
        opened.st_uid == ::geteuid() &&
        opened.st_dev == before.st_dev &&
        opened.st_ino == before.st_ino;
    const bool secured =
        same_directory && owner_private_descriptor(descriptor, true);
    ::close(descriptor);
    return secured;
#endif
}

bool write_private_file_atomic(const std::filesystem::path& destination,
                               std::string_view contents) {
    const auto random = pulp::runtime::secure_random_bytes(8);
    if (!random)
        return false;
    auto temporary = destination;
    temporary += std::filesystem::path(
        ".tmp-" + pulp::runtime::hex_encode(*random)).native();
#ifdef _WIN32
    OwnerOnlySecurity security;
    if (!security.valid())
        return false;
    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0,
        security.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::size_t written = 0;
    bool succeeded = true;
    while (written < contents.size()) {
        const auto remaining = std::min<std::size_t>(
            contents.size() - written,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!WriteFile(file, contents.data() + written,
                       static_cast<DWORD>(remaining), &count, nullptr) ||
            count == 0) {
            succeeded = false;
            break;
        }
        written += count;
    }
    succeeded = succeeded && FlushFileBuffers(file);
    CloseHandle(file);
    if (!succeeded ||
        !MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
#else
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          0600);
    if (fd < 0)
        return false;
    if (!clear_extended_acl(fd) ||
        ::fchmod(fd, 0600) != 0 ||
        !owner_private_descriptor(fd, false)) {
        ::close(fd);
        ::unlink(temporary.c_str());
        return false;
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto count =
            ::write(fd, contents.data() + written, contents.size() - written);
        if (count <= 0) {
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#endif
    return true;
}

OwnershipLease::~OwnershipLease() {
#ifdef _WIN32
    if (native_handle_)
        CloseHandle(static_cast<HANDLE>(native_handle_));
#else
    if (descriptor_ >= 0) {
        ::flock(descriptor_, LOCK_UN);
        ::close(descriptor_);
    }
#endif
}

std::unique_ptr<OwnershipLease> OwnershipLease::acquire(
    const std::filesystem::path& path,
    std::string_view marker) {
    auto lease = std::make_unique<OwnershipLease>();
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        !owner_private_path(path, false))
        return nullptr;
    OwnerOnlySecurity security;
    if (!security.valid())
        return nullptr;
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        security.attributes(), OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    lease->native_handle_ = handle;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return nullptr;
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle))
        return nullptr;
    std::size_t written = 0;
    while (written < marker.size()) {
        const auto remaining = std::min<std::size_t>(
            marker.size() - written,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!WriteFile(handle, marker.data() + written,
                       static_cast<DWORD>(remaining), &count, nullptr) ||
            count == 0)
            return nullptr;
        written += count;
    }
    if (!FlushFileBuffers(handle))
        return nullptr;
#else
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    lease->descriptor_ = ::open(path.c_str(), flags, 0600);
    const bool created = lease->descriptor_ >= 0;
    if (!created && errno == EEXIST) {
        flags &= ~(O_CREAT | O_EXCL);
        lease->descriptor_ = ::open(path.c_str(), flags);
    }
    if (lease->descriptor_ < 0 ||
        ::flock(lease->descriptor_, LOCK_EX | LOCK_NB) != 0)
        return nullptr;
    if ((created &&
         (!clear_extended_acl(lease->descriptor_) ||
          ::fchmod(lease->descriptor_, 0600) != 0)) ||
        !owner_private_descriptor(lease->descriptor_, false) ||
        ::ftruncate(lease->descriptor_, 0) != 0)
        return nullptr;
    std::size_t written = 0;
    while (written < marker.size()) {
        const auto count = ::write(
            lease->descriptor_, marker.data() + written,
            marker.size() - written);
        if (count <= 0)
            return nullptr;
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(lease->descriptor_) != 0)
        return nullptr;
#endif
    return lease;
}

} // namespace pulp::inspect::discovery_security
