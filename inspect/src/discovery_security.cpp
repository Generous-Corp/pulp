#include "discovery_security.hpp"

#include "discovery_internal.hpp"

#include <fstream>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

namespace pulp::inspect::discovery_security {

#ifdef _WIN32
namespace {
class CurrentUserSid {
public:
    CurrentUserSid() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        bytes_.resize(size);
        if (size == 0 ||
            !GetTokenInformation(token, TokenUser, bytes_.data(), size,
                                 &size)) {
            bytes_.clear();
        }
        CloseHandle(token);
    }

    PSID get() const {
        return bytes_.empty()
                   ? nullptr
                   : reinterpret_cast<const TOKEN_USER*>(bytes_.data())
                         ->User.Sid;
    }

private:
    std::vector<std::uint8_t> bytes_;
};
} // namespace

bool owner_private_path(const std::filesystem::path& path,
                        bool expect_directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != expect_directory)
        return false;
    CurrentUserSid expected;
    if (!expected.get())
        return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || !owner || !dacl ||
        !EqualSid(owner, expected.get())) {
        if (descriptor)
            LocalFree(descriptor);
        return false;
    }
    ACL_SIZE_INFORMATION info{};
    bool secure =
        GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) !=
        FALSE;
    for (DWORD index = 0; secure && index < info.AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(dacl, index, &raw)) {
            secure = false;
            break;
        }
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            if (!EqualSid(const_cast<DWORD*>(&ace->SidStart), expected.get()))
                secure = false;
        } else if (
            header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
            header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            secure = false;
        }
    }
    LocalFree(descriptor);
    return secure;
}
#else
namespace {
bool has_no_extended_acl(int descriptor) {
#ifdef __APPLE__
    errno = 0;
    acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
    if (!acl)
        return errno == ENOENT;
    ::acl_free(acl);
    return false;
#else
    (void)descriptor;
    return true;
#endif
}
} // namespace

bool owner_private_descriptor(int descriptor, bool expect_directory) {
    struct stat info {};
    return ::fstat(descriptor, &info) == 0 &&
           (expect_directory ? S_ISDIR(info.st_mode)
                             : S_ISREG(info.st_mode)) &&
           info.st_uid == ::geteuid() &&
           (info.st_mode & 077) == 0 &&
           has_no_extended_acl(descriptor);
}

int open_owner_private(const std::filesystem::path& path,
                       bool expect_directory) {
    struct stat before {};
    if (::lstat(path.c_str(), &before) != 0 ||
        S_ISLNK(before.st_mode) ||
        (expect_directory ? !S_ISDIR(before.st_mode)
                          : !S_ISREG(before.st_mode)))
        return -1;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    if (expect_directory)
        flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    struct stat opened {};
    if (descriptor < 0 ||
        ::fstat(descriptor, &opened) != 0 ||
        opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino ||
        !owner_private_descriptor(descriptor, expect_directory)) {
        if (descriptor >= 0)
            ::close(descriptor);
        return -1;
    }
    return descriptor;
}

bool owner_private_path(const std::filesystem::path& path,
                        bool expect_directory) {
    const int descriptor = open_owner_private(path, expect_directory);
    if (descriptor < 0)
        return false;
    ::close(descriptor);
    return true;
}
#endif

std::optional<std::string> read_private_text_file(
    const std::filesystem::path& path) {
#ifdef _WIN32
    if (!owner_private_path(path, false))
        return std::nullopt;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > discovery_detail::kMaxDiscoveryRecordBytes)
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!contents.empty())
        input.read(contents.data(),
                   static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size()))
        return std::nullopt;
    char trailing = '\0';
    if (input.get(trailing))
        return std::nullopt;
    return contents;
#else
    const int descriptor = open_owner_private(path, false);
    if (descriptor < 0)
        return std::nullopt;
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || info.st_size < 0 ||
        static_cast<std::uintmax_t>(info.st_size) >
            discovery_detail::kMaxDiscoveryRecordBytes) {
        ::close(descriptor);
        return std::nullopt;
    }
    std::string contents(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t read = 0;
    while (read < contents.size()) {
        const auto count =
            ::read(descriptor, contents.data() + read, contents.size() - read);
        if (count <= 0) {
            ::close(descriptor);
            return std::nullopt;
        }
        read += static_cast<std::size_t>(count);
    }
    char trailing = '\0';
    const auto trailing_count = ::read(descriptor, &trailing, 1);
    ::close(descriptor);
    if (trailing_count != 0)
        return std::nullopt;
    return contents;
#endif
}

} // namespace pulp::inspect::discovery_security
