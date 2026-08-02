#include <pulp/project_package/atomic_publisher.hpp>

#include "native_io.hpp"
#include "project_package_test_access.hpp"

#include <pulp/timeline/asset_path.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
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

namespace pulp::project_package {
namespace fs = std::filesystem;

namespace {

template <typename T>
runtime::Result<T, PackageError> failure(PackageErrorCode code, const fs::path& path) {
    return runtime::Result<T, PackageError>(runtime::Err(PackageError{code, path}));
}

fs::path staging_sibling(const fs::path& destination, std::uint64_t serial) {
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = ".";
    const auto seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return parent / (".pulp-staging-" + std::to_string(seed ^ serial));
}

fs::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return fs::path(value);
#endif
}

enum class PrivateDirectoryCreate : std::uint8_t { Created, AlreadyExists, Failed };

bool parent_allows_private_staging(const fs::path& parent) noexcept {
#if defined(_WIN32)
    const auto directory = CreateFileW(
        parent.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool real_directory = GetFileInformationByHandle(directory, &info) != 0 &&
                                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto security_error = GetSecurityInfo(directory, SE_FILE_OBJECT,
                                                OWNER_SECURITY_INFORMATION |
                                                    DACL_SECURITY_INFORMATION,
                                                &owner, nullptr, &dacl, nullptr, &descriptor);
    CloseHandle(directory);
    if (!real_directory || security_error != ERROR_SUCCESS || owner == nullptr || dacl == nullptr) {
        if (descriptor != nullptr)
            LocalFree(descriptor);
        return false;
    }

    HANDLE token = nullptr;
    DWORD token_bytes = 0;
    bool token_opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != 0;
    if (!token_opened && GetLastError() == ERROR_NO_TOKEN)
        token_opened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0;
    if (token_opened)
        GetTokenInformation(token, TokenUser, nullptr, 0, &token_bytes);
    std::vector<std::uint8_t> token_storage(token_bytes);
    const bool token_read = token_opened && token_bytes != 0 &&
                            GetTokenInformation(token, TokenUser, token_storage.data(),
                                                token_bytes, &token_bytes) != 0;
    if (token != nullptr)
        CloseHandle(token);
    const auto* token_user =
        token_read ? reinterpret_cast<const TOKEN_USER*>(token_storage.data()) : nullptr;

    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_storage{};
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> administrators_storage{};
    DWORD system_bytes = static_cast<DWORD>(system_storage.size());
    DWORD administrators_bytes = static_cast<DWORD>(administrators_storage.size());
    const bool trusted_sids =
        CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_bytes) != 0 &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_storage.data(),
                           &administrators_bytes) != 0;
    bool safe = token_user != nullptr && trusted_sids && EqualSid(owner, token_user->User.Sid) != 0;
    constexpr ACCESS_MASK dangerous = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD |
                                      DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD index = 0; safe && index < dacl->AceCount; ++index) {
        void* raw = nullptr;
        if (GetAce(dacl, index, &raw) == 0) {
            safe = false;
            break;
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
            continue;
        ACCESS_MASK mask = 0;
        PSID sid = nullptr;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
        } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(raw);
            mask = ace->Mask;
            auto* cursor = reinterpret_cast<const std::uint8_t*>(&ace->ObjectType);
            if ((ace->Flags & ACE_OBJECT_TYPE_PRESENT) != 0)
                cursor += sizeof(GUID);
            if ((ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) != 0)
                cursor += sizeof(GUID);
            sid = const_cast<std::uint8_t*>(cursor);
        } else if (header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                   header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            // Conditional grants are deliberately rejected when they might grant
            // namespace control; evaluating arbitrary AuthZ expressions here would
            // make staging admission dependent on ambient claims.
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if ((ace->Mask & dangerous) != 0)
                safe = false;
            continue;
        } else {
            continue;
        }
        if ((mask & dangerous) == 0)
            continue;
        if (sid == nullptr || IsValidSid(sid) == 0) {
            safe = false;
            continue;
        }
        const bool trusted = EqualSid(sid, token_user->User.Sid) != 0 ||
                             EqualSid(sid, system_storage.data()) != 0 ||
                             EqualSid(sid, administrators_storage.data()) != 0;
        if (!trusted)
            safe = false;
    }
    LocalFree(descriptor);
    return safe;
#else
    struct stat status{};
    if (::stat(parent.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return false;
    const bool writable_by_others = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    if (writable_by_others) {
        const bool trusted_sticky_parent = (status.st_mode & S_ISVTX) != 0 &&
                                           (status.st_uid == ::geteuid() || status.st_uid == 0);
        if (!trusted_sticky_parent)
            return false;
    }
#if defined(__APPLE__)
    errno = 0;
    acl_t acl = ::acl_get_file(parent.c_str(), ACL_TYPE_EXTENDED);
    if (acl == nullptr)
        return errno == ENOENT || errno == EOPNOTSUPP;
    bool safe = true;
    acl_entry_t entry = nullptr;
    int entry_result = ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    while (entry_result == 0 && safe) {
        acl_tag_t tag = ACL_UNDEFINED_TAG;
        acl_flagset_t flags = nullptr;
        if (::acl_get_tag_type(entry, &tag) != 0 ||
            ::acl_get_flagset_np(entry, &flags) != 0) {
            safe = false;
            break;
        }
        const int only_inherit = ::acl_get_flag_np(flags, ACL_ENTRY_ONLY_INHERIT);
        acl_permset_mask_t mask = 0;
        if (only_inherit < 0 || ::acl_get_permset_mask_np(entry, &mask) != 0) {
            safe = false;
            break;
        }
        const bool applies_to_parent = only_inherit == 0;
        constexpr acl_permset_mask_t dangerous_mask =
            ACL_ADD_FILE | ACL_ADD_SUBDIRECTORY | ACL_DELETE_CHILD | ACL_DELETE |
            ACL_WRITE_SECURITY | ACL_CHANGE_OWNER;
        const bool dangerous = (mask & dangerous_mask) != 0;
        if (tag == ACL_EXTENDED_ALLOW && applies_to_parent && dangerous)
            safe = false;
        entry_result = ::acl_get_entry(acl, ACL_NEXT_ENTRY, &entry);
    }
    if (entry_result < 0 && errno != EINVAL)
        safe = false;
    ::acl_free(acl);
    return safe;
#else
    return true;
#endif
#endif
}

#if defined(_WIN32)
bool private_directory_security_matches(HANDLE directory) noexcept {
    PSECURITY_DESCRIPTOR expected_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1,
            &expected_descriptor, nullptr))
        return false;

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
        directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
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

PrivateDirectoryCreate create_private_directory(const fs::path& path) noexcept {
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor,
            nullptr))
        return PrivateDirectoryCreate::Failed;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const bool created = CreateDirectoryW(path.c_str(), &attributes) != 0;
    const auto create_error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!created)
        return create_error == ERROR_FILE_EXISTS || create_error == ERROR_ALREADY_EXISTS
                   ? PrivateDirectoryCreate::AlreadyExists
                   : PrivateDirectoryCreate::Failed;

    HANDLE directory = CreateFileW(
        path.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const bool private_security =
        directory != INVALID_HANDLE_VALUE && private_directory_security_matches(directory);
    if (directory != INVALID_HANDLE_VALUE)
        CloseHandle(directory);
    if (private_security)
        return PrivateDirectoryCreate::Created;
    std::error_code ignored;
    fs::remove(path, ignored);
    return PrivateDirectoryCreate::Failed;
#else
    if (::mkdir(path.c_str(), 0700) != 0)
        return errno == EEXIST ? PrivateDirectoryCreate::AlreadyExists
                               : PrivateDirectoryCreate::Failed;
    const int directory = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) {
        std::error_code ignored;
        fs::remove(path, ignored);
        return PrivateDirectoryCreate::Failed;
    }
    bool private_security = ::fchmod(directory, 0700) == 0;
#if defined(__APPLE__)
    acl_t empty = ::acl_init(0);
    errno = 0;
    const bool acl_cleared =
        empty != nullptr && ::acl_set_fd_np(directory, empty, ACL_TYPE_EXTENDED) == 0;
    const bool acl_unsupported = !acl_cleared && errno == EOPNOTSUPP;
    private_security = private_security && (acl_cleared || acl_unsupported);
    if (empty != nullptr)
        ::acl_free(empty);
#elif defined(__linux__)
    for (const char* name : {"system.posix_acl_access", "system.posix_acl_default"})
        if (::fremovexattr(directory, name) != 0 && errno != ENODATA && errno != ENOTSUP)
            private_security = false;
#endif
    struct stat status{};
    private_security = private_security && ::fstat(directory, &status) == 0 &&
                       S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0700 &&
                       status.st_uid == ::geteuid();
    ::close(directory);
    if (private_security)
        return PrivateDirectoryCreate::Created;
    std::error_code ignored;
    fs::remove(path, ignored);
    return PrivateDirectoryCreate::Failed;
#endif
}

} // namespace

struct AtomicPublisher::Impl {
    fs::path destination;
    fs::path staging;
    detail::AnchoredDirectory parent_root;
    detail::AnchoredDirectory staging_root;
    bool committed = false;
};

AtomicPublisher::AtomicPublisher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AtomicPublisher::~AtomicPublisher() {
    cancel();
}
AtomicPublisher::AtomicPublisher(AtomicPublisher&&) noexcept = default;
AtomicPublisher& AtomicPublisher::operator=(AtomicPublisher&& other) noexcept {
    if (this != &other) {
        cancel();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

runtime::Result<AtomicPublisher, PackageError>
AtomicPublisher::create(const fs::path& destination) noexcept {
    if (destination.empty())
        return failure<AtomicPublisher>(PackageErrorCode::InvalidPath, destination);
    std::error_code error;
    const auto absolute_destination = fs::absolute(destination, error);
    if (error)
        return failure<AtomicPublisher>(PackageErrorCode::InvalidPath, destination);

    const auto parent = fs::canonical(absolute_destination.parent_path(), error);
    if (error || fs::status(parent, error).type() != fs::file_type::directory ||
        !parent_allows_private_staging(parent))
        return failure<AtomicPublisher>(PackageErrorCode::InvalidPath,
                                        absolute_destination.parent_path());
    auto parent_root = detail::AnchoredDirectory::open(parent);
    if (!parent_root || !parent_root->still_named_by(parent))
        return failure<AtomicPublisher>(PackageErrorCode::InvalidPath, parent);
    const auto anchored_destination = parent / absolute_destination.filename();
    const auto status = fs::symlink_status(anchored_destination, error);
    if ((!error && status.type() != fs::file_type::not_found) ||
        (error && error != std::errc::no_such_file_or_directory))
        return failure<AtomicPublisher>(PackageErrorCode::PublicationConflict,
                                        anchored_destination);
    error.clear();

    static std::atomic<std::uint64_t> serial{0};
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
        auto staging = staging_sibling(anchored_destination, serial.fetch_add(1) + attempt);
        const auto created = create_private_directory(staging);
        if (created == PrivateDirectoryCreate::Created) {
            auto staging_root = parent_root->open_directory(staging.filename(), true);
            if (!staging_root || !staging_root->still_named_by(staging)) {
                fs::remove(staging, error);
                return failure<AtomicPublisher>(PackageErrorCode::IoError, staging);
            }
            auto impl = std::make_unique<Impl>();
            impl->destination = anchored_destination;
            impl->staging = std::move(staging);
            impl->parent_root = std::move(*parent_root);
            impl->staging_root = std::move(*staging_root);
            return runtime::Result<AtomicPublisher, PackageError>(
                runtime::Ok(AtomicPublisher(std::move(impl))));
        }
        if (created == PrivateDirectoryCreate::AlreadyExists)
            continue;
        return failure<AtomicPublisher>(PackageErrorCode::IoError, staging);
    }
    return failure<AtomicPublisher>(PackageErrorCode::PublicationConflict, anchored_destination);
}

const fs::path& AtomicPublisher::staging_directory() const noexcept {
    static const fs::path empty;
    return impl_ ? impl_->staging : empty;
}

runtime::Result<bool, PackageError>
AtomicPublisher::write(std::string_view relative_utf8,
                       std::span<const std::uint8_t> bytes) noexcept {
    if (!impl_ || impl_->committed ||
        !timeline::package_relative_path_is_lexically_safe(relative_utf8))
        return failure<bool>(PackageErrorCode::InvalidPath, impl_ ? impl_->staging : fs::path{});
    const fs::path relative = path_from_utf8(relative_utf8);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory())
        return failure<bool>(PackageErrorCode::InvalidPath, relative);
    const auto output = impl_->staging / relative;
    if (!impl_->staging_root.write_exclusive_and_fence(relative, bytes,
                                                       detail::PackageFaultPoint::StagedFileWritten,
                                                       detail::PackageFaultPoint::StagedFileFenced))
        return failure<bool>(PackageErrorCode::IoError, output);
    return runtime::Result<bool, PackageError>(runtime::Ok(true));
}

runtime::Result<bool, PackageError> AtomicPublisher::write(std::string_view relative_utf8,
                                                           std::string_view text) noexcept {
    return write(relative_utf8,
                 std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                               text.size()));
}

runtime::Result<AtomicPublishOutcome, PackageError>
AtomicPublisher::commit_file(const fs::path& staged_file) noexcept {
    if (!impl_ || impl_->committed || staged_file.parent_path() != impl_->staging ||
        !detail::regular_file_no_links(staged_file))
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidPath, staged_file);
    auto pinned = detail::PinnedFile::open(staged_file, true, true);
    if (!pinned || !pinned->fence() || !pinned->still_named_by(staged_file) ||
        !impl_->staging_root.still_named_by(impl_->staging))
        return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, staged_file);
    detail::invoke_fault_hook(detail::PackageFaultPoint::StagedFileFenced);
    if (!pinned->still_named_by(staged_file) ||
        !impl_->staging_root.still_named_by(impl_->staging))
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, staged_file);
    detail::invoke_fault_hook(detail::PackageFaultPoint::PublicationSourceVerified);
    const auto publication = impl_->staging_root.publish_no_replace(
        staged_file.filename(), impl_->parent_root, impl_->destination.filename(),
        detail::NoReplaceSourceKind::RegularFile);
    if (publication == detail::NoReplaceOutcome::DestinationExists)
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::NotPublished));
    if (publication != detail::NoReplaceOutcome::Published)
        return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, impl_->destination);
    impl_->committed = true;
    impl_->staging_root.close();
    if (!pinned->still_named_by(impl_->destination))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    detail::invoke_fault_hook(detail::PackageFaultPoint::DirectoryPublished);
    std::error_code ignored;
    fs::remove(impl_->staging, ignored);
    if (!impl_->parent_root.fence())
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    return runtime::Result<AtomicPublishOutcome, PackageError>(
        runtime::Ok(AtomicPublishOutcome::PublishedDurably));
}

runtime::Result<AtomicPublishOutcome, PackageError> AtomicPublisher::commit_directory() noexcept {
    if (!impl_ || impl_->committed)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, {});
    if (!impl_->staging_root.still_named_by(impl_->staging))
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, impl_->staging);
    std::vector<fs::path> directories;
    std::vector<fs::path> files;
    std::vector<detail::AnchoredDirectory> pinned_directories;
    std::vector<detail::PinnedFile> pinned_files;
    directories.push_back(impl_->staging);
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(impl_->staging, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (status.type() == fs::file_type::symlink)
            return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, iterator->path());
        if (error)
            break;
        if (status.type() == fs::file_type::directory)
            directories.push_back(iterator->path());
        else if (status.type() == fs::file_type::regular)
            files.push_back(iterator->path());
        else
            error = std::make_error_code(std::errc::invalid_argument);
    }
    if (error)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, impl_->staging);
    for (const auto& file : files) {
        auto pinned = detail::PinnedFile::open(file, true, true);
        if (!pinned || !pinned->fence() || !pinned->still_named_by(file))
            return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, file);
        pinned_files.push_back(std::move(*pinned));
    }
    std::sort(directories.begin(), directories.end(), [](const auto& lhs, const auto& rhs) {
        return std::distance(lhs.begin(), lhs.end()) > std::distance(rhs.begin(), rhs.end());
    });
    for (const auto& directory : directories) {
        auto pinned = detail::AnchoredDirectory::open(directory, true);
        if (!pinned || !detail::fence_directory(directory) || !pinned->still_named_by(directory))
            return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, directory);
        pinned_directories.push_back(std::move(*pinned));
    }
    if (!impl_->staging_root.still_named_by(impl_->staging))
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, impl_->staging);
    detail::invoke_fault_hook(detail::PackageFaultPoint::DirectoryTreeFenced);
    for (std::size_t index = 0; index < files.size(); ++index)
        if (!pinned_files[index].still_named_by(files[index]))
            return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, files[index]);
    for (std::size_t index = 0; index < directories.size(); ++index)
        if (!pinned_directories[index].still_named_by(directories[index]))
            return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout,
                                                 directories[index]);
    detail::invoke_fault_hook(detail::PackageFaultPoint::PublicationSourceVerified);
    const auto publication = impl_->parent_root.publish_no_replace(
        impl_->staging.filename(), impl_->parent_root, impl_->destination.filename(),
        detail::NoReplaceSourceKind::Directory);
    if (publication == detail::NoReplaceOutcome::DestinationExists)
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::NotPublished));
    if (publication != detail::NoReplaceOutcome::Published)
        return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, impl_->destination);
    impl_->committed = true;
    if (!impl_->staging_root.still_named_by(impl_->destination))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    impl_->staging_root.close();
    detail::invoke_fault_hook(detail::PackageFaultPoint::DirectoryPublished);
    if (!impl_->parent_root.fence())
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    return runtime::Result<AtomicPublishOutcome, PackageError>(
        runtime::Ok(AtomicPublishOutcome::PublishedDurably));
}

void AtomicPublisher::cancel() noexcept {
    if (!impl_ || impl_->committed || impl_->staging.empty())
        return;
    std::error_code ignored;
    if (impl_->staging_root.still_named_by(impl_->staging)) {
        impl_->staging_root.close();
        fs::remove_all(impl_->staging, ignored);
    } else {
        // Leaking an unreachable private stage is safer than recursively deleting
        // an unrelated object that has rebound to the old staging pathname.
        impl_->staging_root.close();
    }
    impl_->staging.clear();
}

} // namespace pulp::project_package
