#include <pulp/runtime/memory_mapped_file.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <utility>

#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pulp::runtime {

AccessPolicyTarget::~AccessPolicyTarget() {
#ifdef _WIN32
    if (handle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(handle_));
    if (descriptor_ != nullptr)
        LocalFree(descriptor_);
#else
    if (fd_ >= 0)
        ::close(fd_);
#endif
}

AccessPolicyTarget::AccessPolicyTarget(AccessPolicyTarget&& other) noexcept {
    *this = std::move(other);
}

AccessPolicyTarget& AccessPolicyTarget::operator=(AccessPolicyTarget&& other) noexcept {
    if (this == &other)
        return *this;
#ifdef _WIN32
    if (handle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(handle_));
    if (descriptor_ != nullptr)
        LocalFree(descriptor_);
    handle_ = std::exchange(other.handle_, nullptr);
    descriptor_ = std::exchange(other.descriptor_, nullptr);
    dacl_ = std::exchange(other.dacl_, nullptr);
    source_dacl_protected_ = other.source_dacl_protected_;
#else
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
#endif
    return *this;
}

AccessPolicyTarget::operator bool() const noexcept {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

bool AccessPolicyTarget::finalize_after_move() noexcept {
#ifdef _WIN32
    if (handle_ == nullptr)
        return false;
    if (source_dacl_protected_)
        return true;
    return SetSecurityInfo(static_cast<HANDLE>(handle_), SE_FILE_OBJECT,
                           DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                           nullptr, nullptr, static_cast<PACL>(dacl_), nullptr) == ERROR_SUCCESS;
#else
    return fd_ >= 0;
#endif
}

bool AccessPolicyTarget::sync() noexcept {
#ifdef _WIN32
    return handle_ != nullptr && FlushFileBuffers(static_cast<HANDLE>(handle_)) != 0;
#else
    return fd_ >= 0 && ::fsync(fd_) == 0;
#endif
}

MemoryMappedFile::~MemoryMappedFile() {
    close();
}

MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_)
#ifdef _WIN32
      ,
      file_handle_(other.file_handle_), mapping_handle_(other.mapping_handle_)
#else
      ,
      fd_(other.fd_)
#endif
{
    other.data_ = nullptr;
    other.size_ = 0;
#ifdef _WIN32
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
#else
    other.fd_ = -1;
#endif
}

MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        other.file_handle_ = nullptr;
        other.mapping_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

#ifdef _WIN32

namespace {

std::uint64_t windows_file_generation(const BY_HANDLE_FILE_INFORMATION& information) noexcept {
    auto generation = (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32) |
                      information.ftLastWriteTime.dwLowDateTime;
    const auto size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
                      information.nFileSizeLow;
    generation ^= size + 0x9e3779b97f4a7c15ull + (generation << 6) + (generation >> 2);
    return generation;
}

} // namespace

bool MemoryMappedFile::open(std::string_view path, MapMode mode) {
    return open(path, mode, std::numeric_limits<std::size_t>::max());
}

bool MemoryMappedFile::open(std::string_view path, MapMode mode, std::size_t maximum_bytes) {
    return open_impl(path, mode, maximum_bytes, false);
}

bool MemoryMappedFile::open_no_follow(std::string_view path, MapMode mode,
                                      std::size_t maximum_bytes) {
    return open_impl(path, mode, maximum_bytes, true);
}

bool MemoryMappedFile::open_impl(std::string_view path, MapMode mode, std::size_t maximum_bytes,
                                 bool no_follow) {
    close();

    DWORD access = (mode == MapMode::ReadWrite) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD share = FILE_SHARE_READ;
    if (mode == MapMode::ReadOnly)
        share |= FILE_SHARE_DELETE;
    std::string path_str(path);

    const DWORD attributes = FILE_ATTRIBUTE_NORMAL |
                             (no_follow ? FILE_FLAG_OPEN_REPARSE_POINT : DWORD{0});
    file_handle_ =
        CreateFileA(path_str.c_str(), access, share, nullptr, OPEN_EXISTING, attributes, nullptr);
    if (file_handle_ == INVALID_HANDLE_VALUE) {
        file_handle_ = nullptr;
        return false;
    }
    BY_HANDLE_FILE_INFORMATION opened_information{};
    if (!GetFileInformationByHandle(static_cast<HANDLE>(file_handle_), &opened_information) ||
        (opened_information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (no_follow &&
         (opened_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
        close();
        return false;
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file_handle_, &file_size)) {
        close();
        return false;
    }
    if (file_size.QuadPart <= 0 || static_cast<unsigned long long>(file_size.QuadPart) >

                                       maximum_bytes) {
        close();
        return false;
    }
    size_ = static_cast<size_t>(file_size.QuadPart);

    DWORD protect = (mode == MapMode::ReadWrite) ? PAGE_READWRITE : PAGE_READONLY;
    mapping_handle_ = CreateFileMappingA(file_handle_, nullptr, protect, 0, 0, nullptr);
    if (!mapping_handle_) {
        close();
        return false;
    }

    DWORD map_access = (mode == MapMode::ReadWrite) ? FILE_MAP_WRITE : FILE_MAP_READ;
    data_ = static_cast<uint8_t*>(MapViewOfFile(mapping_handle_, map_access, 0, 0, 0));
    if (!data_) {
        close();
        return false;
    }

    return true;
}

void MemoryMappedFile::close() {
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
    }
    size_ = 0;
}

bool MemoryMappedFile::copy_access_policy_to(std::string_view destination) const noexcept {
    if (file_handle_ == nullptr)
        return false;
    std::string destination_string;
    try {
        destination_string.assign(destination);
    } catch (...) {
        return false;
    }

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto read_status =
        GetSecurityInfo(static_cast<HANDLE>(file_handle_), SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (read_status != ERROR_SUCCESS || descriptor == nullptr)
        return false;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        LocalFree(descriptor);
        return false;
    }

    HANDLE target = CreateFileA(destination_string.c_str(),
                                GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target == INVALID_HANDLE_VALUE) {
        LocalFree(descriptor);
        return false;
    }
    SECURITY_INFORMATION information = DACL_SECURITY_INFORMATION;
    information |= (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                 : UNPROTECTED_DACL_SECURITY_INFORMATION;
    const auto write_status =
        SetSecurityInfo(target, SE_FILE_OBJECT, information, nullptr, nullptr, dacl, nullptr);
    CloseHandle(target);
    LocalFree(descriptor);
    return write_status == ERROR_SUCCESS;
}

AccessPolicyTarget
MemoryMappedFile::prepare_access_policy_target(std::string_view destination) const noexcept {
    AccessPolicyTarget result;
    if (file_handle_ == nullptr)
        return result;
    std::string destination_string;
    try {
        destination_string.assign(destination);
    } catch (...) {
        return result;
    }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(static_cast<HANDLE>(file_handle_), SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                        &descriptor) != ERROR_SUCCESS ||
        descriptor == nullptr)
        return result;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        LocalFree(descriptor);
        return result;
    }
    HANDLE target = CreateFileA(destination_string.c_str(),
                                GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target == INVALID_HANDLE_VALUE ||
        SetSecurityInfo(target, SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                        nullptr, dacl, nullptr) != ERROR_SUCCESS) {
        if (target != INVALID_HANDLE_VALUE)
            CloseHandle(target);
        LocalFree(descriptor);
        return result;
    }
    result.handle_ = target;
    result.descriptor_ = descriptor;
    result.dacl_ = dacl;
    result.source_dacl_protected_ = (control & SE_DACL_PROTECTED) != 0;
    return result;
}

bool MemoryMappedFile::path_refers_to_open_file(std::string_view path) const noexcept {
    if (file_handle_ == nullptr)
        return false;
    BY_HANDLE_FILE_INFORMATION opened{};
    if (!GetFileInformationByHandle(static_cast<HANDLE>(file_handle_), &opened))
        return false;
    std::string path_string;
    try {
        path_string.assign(path);
    } catch (...) {
        return false;
    }
    HANDLE current = CreateFileA(path_string.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (current == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION resolved{};
    const bool read = GetFileInformationByHandle(current, &resolved) != 0;
    CloseHandle(current);
    return read && opened.dwVolumeSerialNumber == resolved.dwVolumeSerialNumber &&
           opened.nFileIndexHigh == resolved.nFileIndexHigh &&
           opened.nFileIndexLow == resolved.nFileIndexLow &&
           windows_file_generation(opened) == windows_file_generation(resolved);
}

FileIdentity MemoryMappedFile::opened_file_identity() const noexcept {
    FileIdentity result;
    if (file_handle_ == nullptr)
        return result;
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(static_cast<HANDLE>(file_handle_), &information))
        return result;
    result.volume = information.dwVolumeSerialNumber;
    result.file =
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
    result.generation = windows_file_generation(information);
    result.valid = true;
    return result;
}

bool MemoryMappedFile::copy_contents_to_new_file(std::string_view destination) const noexcept {
    if (file_handle_ == nullptr)
        return false;
    std::string destination_string;
    try {
        destination_string.assign(destination);
    } catch (...) {
        return false;
    }
    HANDLE target = CreateFileA(destination_string.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER beginning{};
    bool ok = SetFilePointerEx(static_cast<HANDLE>(file_handle_), beginning, nullptr, FILE_BEGIN) !=
              0;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::size_t remaining = size_;
    while (ok && remaining != 0) {
        const auto request = static_cast<DWORD>(std::min(remaining, buffer.size()));
        DWORD read = 0;
        ok = ReadFile(static_cast<HANDLE>(file_handle_), buffer.data(), request, &read, nullptr) !=
                 0 &&
             read == request;
        DWORD written = 0;
        if (ok)
            ok = WriteFile(target, buffer.data(), read, &written, nullptr) != 0 && written == read;
        remaining -= ok ? read : 0;
    }
    CloseHandle(target);
    if (!ok)
        DeleteFileA(destination_string.c_str());
    return ok;
}

FileIdentity file_identity(std::string_view path) noexcept {
    std::string path_string;
    try {
        path_string.assign(path);
    } catch (...) {
        return {};
    }
    HANDLE handle = CreateFileA(path_string.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION information{};
    const bool read = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!read)
        return {};
    return {.volume = information.dwVolumeSerialNumber,
            .file = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                    information.nFileIndexLow,
            .generation = windows_file_generation(information),
            .valid = true};
}

#else // POSIX

bool MemoryMappedFile::open(std::string_view path, MapMode mode) {
    return open(path, mode, std::numeric_limits<std::size_t>::max());
}

bool MemoryMappedFile::open(std::string_view path, MapMode mode, std::size_t maximum_bytes) {
    return open_impl(path, mode, maximum_bytes, false);
}

bool MemoryMappedFile::open_no_follow(std::string_view path, MapMode mode,
                                      std::size_t maximum_bytes) {
    return open_impl(path, mode, maximum_bytes, true);
}

bool MemoryMappedFile::open_impl(std::string_view path, MapMode mode, std::size_t maximum_bytes,
                                 bool no_follow) {
    close();

    int flags = (mode == MapMode::ReadWrite) ? O_RDWR : O_RDONLY;
    if (no_follow)
        flags |= O_NOFOLLOW;
    std::string path_str(path);
    fd_ = ::open(path_str.c_str(), flags);
    if (fd_ < 0)
        return false;

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        close();
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        close();
        return false;
    }
    if (st.st_size <= 0 || static_cast<std::uintmax_t>(st.st_size) >

                               maximum_bytes) {
        close();
        return false;
    }
    size_ = static_cast<size_t>(st.st_size);

    int prot = (mode == MapMode::ReadWrite) ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* ptr = mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
    if (ptr == MAP_FAILED) {
        close();
        return false;
    }

    data_ = static_cast<uint8_t*>(ptr);
    return true;
}

void MemoryMappedFile::close() {
    if (data_) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

bool MemoryMappedFile::copy_access_policy_to(std::string_view destination) const noexcept {
    if (fd_ < 0)
        return false;
    std::string destination_string;
    try {
        destination_string.assign(destination);
    } catch (...) {
        return false;
    }

    struct stat source_status{};
    if (::fstat(fd_, &source_status) != 0)
        return false;

    const int target = ::open(destination_string.c_str(), O_RDONLY | O_NOFOLLOW);
    if (target < 0)
        return false;
    const auto close_target = [&] { ::close(target); };

    if (::fchown(target, source_status.st_uid, source_status.st_gid) != 0 ||
        ::fchmod(target, source_status.st_mode & 07777) != 0) {
        close_target();
        return false;
    }

#if defined(__APPLE__)
    errno = 0;
    acl_t acl = ::acl_get_fd_np(fd_, ACL_TYPE_EXTENDED);
    const int source_acl_error = errno;
    const bool source_has_no_acl =
        acl == nullptr && (source_acl_error == ENOENT || source_acl_error == EOPNOTSUPP);
    if (acl == nullptr && !source_has_no_acl) {
        close_target();
        return false;
    }
    acl_t policy = acl;
    bool acl_ok = source_acl_error == EOPNOTSUPP;
    if (source_has_no_acl && source_acl_error != EOPNOTSUPP)
        policy = ::acl_init(0);
    if (!acl_ok)
        acl_ok = policy != nullptr && ::acl_set_fd_np(target, policy, ACL_TYPE_EXTENDED) == 0;
    if (source_has_no_acl && source_acl_error != EOPNOTSUPP && policy != nullptr)
        ::acl_free(policy);
    if (acl != nullptr)
        ::acl_free(acl);
    if (!acl_ok) {
        close_target();
        return false;
    }
#elif defined(__linux__)
    constexpr const char* kAccessAcl = "system.posix_acl_access";
    errno = 0;
    const auto acl_size = ::fgetxattr(fd_, kAccessAcl, nullptr, 0);
    if (acl_size < 0 && errno != ENODATA && errno != ENOTSUP) {
        close_target();
        return false;
    }
    if (acl_size > 0) {
        const auto size = static_cast<std::size_t>(acl_size);
        auto acl = std::unique_ptr<unsigned char[]>(new (std::nothrow) unsigned char[size]);
        if (!acl || ::fgetxattr(fd_, kAccessAcl, acl.get(), size) != acl_size ||
            ::fsetxattr(target, kAccessAcl, acl.get(), size, 0) != 0) {
            close_target();
            return false;
        }
    } else {
        if (::fremovexattr(target, kAccessAcl) != 0 && errno != ENODATA && errno != ENOTSUP) {
            close_target();
            return false;
        }
    }
#endif

    close_target();
    return true;
}

AccessPolicyTarget
MemoryMappedFile::prepare_access_policy_target(std::string_view destination) const noexcept {
    AccessPolicyTarget result;
    if (!copy_access_policy_to(destination))
        return result;
    std::string destination_string;
    try {
        destination_string.assign(destination);
    } catch (...) {
        return result;
    }
    result.fd_ = ::open(destination_string.c_str(), O_RDONLY | O_NOFOLLOW);
    return result;
}

bool MemoryMappedFile::path_refers_to_open_file(std::string_view path) const noexcept {
    if (fd_ < 0)
        return false;
    struct stat opened{};
    struct stat resolved{};
    std::string path_string;
    try {
        path_string.assign(path);
    } catch (...) {
        return false;
    }
    return ::fstat(fd_, &opened) == 0 && ::stat(path_string.c_str(), &resolved) == 0 &&
           opened.st_dev == resolved.st_dev && opened.st_ino == resolved.st_ino;
}

FileIdentity MemoryMappedFile::opened_file_identity() const noexcept {
    struct stat status{};
    if (fd_ < 0 || ::fstat(fd_, &status) != 0)
        return {};
#if defined(__APPLE__)
    const auto generation_seconds = status.st_ctimespec.tv_sec;
    const auto generation_nanos = status.st_ctimespec.tv_nsec;
#else
    const auto generation_seconds = status.st_ctim.tv_sec;
    const auto generation_nanos = status.st_ctim.tv_nsec;
#endif
    auto generation = static_cast<std::uint64_t>(generation_seconds);
    generation ^= static_cast<std::uint64_t>(generation_nanos) + 0x9e3779b97f4a7c15ull +
                  (generation << 6) + (generation >> 2);
#if defined(__APPLE__)
    generation ^= static_cast<std::uint64_t>(status.st_gen) + 0x9e3779b97f4a7c15ull +
                  (generation << 6) + (generation >> 2);
#endif
    return {.volume = static_cast<std::uint64_t>(status.st_dev),
            .file = static_cast<std::uint64_t>(status.st_ino),
            .generation = generation,
            .valid = true};
}

bool MemoryMappedFile::copy_contents_to_new_file(std::string_view destination) const noexcept {
    if (fd_ < 0)
        return false;
    std::string destination_string;
    try {
        destination_string.assign(destination);
    } catch (...) {
        return false;
    }
    const int target =
        ::open(destination_string.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (target < 0)
        return false;

    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::size_t copied = 0;
    bool ok = true;
    while (copied != size_) {
        const auto request = std::min(size_ - copied, buffer.size());
        ssize_t read = -1;
        do {
            read = ::pread(fd_, buffer.data(), request, static_cast<off_t>(copied));
        } while (read < 0 && errno == EINTR);
        if (read <= 0) {
            ok = false;
            break;
        }
        std::size_t written = 0;
        while (written != static_cast<std::size_t>(read)) {
            ssize_t result = -1;
            do {
                result = ::write(target, buffer.data() + written,
                                 static_cast<std::size_t>(read) - written);
            } while (result < 0 && errno == EINTR);
            if (result <= 0) {
                ok = false;
                break;
            }
            written += static_cast<std::size_t>(result);
        }
        if (!ok)
            break;
        copied += static_cast<std::size_t>(read);
    }
    ok = ::close(target) == 0 && ok;
    if (!ok)
        ::unlink(destination_string.c_str());
    return ok;
}

FileIdentity file_identity(std::string_view path) noexcept {
    struct stat status{};
    std::string path_string;
    try {
        path_string.assign(path);
    } catch (...) {
        return {};
    }
    if (::stat(path_string.c_str(), &status) != 0)
        return {};
#if defined(__APPLE__)
    const auto generation_seconds = status.st_ctimespec.tv_sec;
    const auto generation_nanos = status.st_ctimespec.tv_nsec;
#else
    const auto generation_seconds = status.st_ctim.tv_sec;
    const auto generation_nanos = status.st_ctim.tv_nsec;
#endif
    auto generation = static_cast<std::uint64_t>(generation_seconds);
    generation ^= static_cast<std::uint64_t>(generation_nanos) + 0x9e3779b97f4a7c15ull +
                  (generation << 6) + (generation >> 2);
#if defined(__APPLE__)
    generation ^= static_cast<std::uint64_t>(status.st_gen) + 0x9e3779b97f4a7c15ull +
                  (generation << 6) + (generation >> 2);
#endif
    return {.volume = static_cast<std::uint64_t>(status.st_dev),
            .file = static_cast<std::uint64_t>(status.st_ino),
            .generation = generation,
            .valid = true};
}

#endif

} // namespace pulp::runtime
