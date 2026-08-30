#include "gpu_artifact_publication.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace pulp::cli::gpu_artifacts {
namespace {

namespace fs = std::filesystem;

std::atomic<std::uint64_t> next_temporary_id{0};

std::string temporary_name() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sequence = next_temporary_id.fetch_add(1, std::memory_order_relaxed);
    return ".pulp-gpu-artifact.tmp." + std::to_string(tick) + "." + std::to_string(sequence);
}

fs::path checked_artifact_name(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos)
        throw std::runtime_error("probe returned an unsafe artifact name");
    const fs::path relative{std::string{name}};
    if (relative.is_absolute() || relative.has_parent_path() || relative.filename() != relative ||
        relative == "." || relative == "..") {
        throw std::runtime_error("probe returned an unsafe artifact name");
    }
#if defined(_WIN32)
    if (name.find_first_of("<>:\"/\\|?*") != std::string_view::npos || name.back() == '.' ||
        name.back() == ' ') {
        throw std::runtime_error("probe returned an unsafe Windows artifact name");
    }
#endif
    return relative;
}

#if defined(_WIN32)

using NtCreateFileFunction = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                              PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                              ULONG, PVOID, ULONG);
using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                                      FILE_INFORMATION_CLASS);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(NTSTATUS);

constexpr ULONG object_dont_reparse = 0x00001000L;

NtCreateFileFunction nt_create_file() noexcept {
    static const auto function = reinterpret_cast<NtCreateFileFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    return function;
}

NtSetInformationFileFunction nt_set_information_file() noexcept {
    static const auto function = reinterpret_cast<NtSetInformationFileFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    return function;
}

DWORD windows_error_from_status(NTSTATUS status) noexcept {
    static const auto function = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    return function != nullptr ? function(status) : ERROR_GEN_FAILURE;
}

[[noreturn]] void throw_windows_error(const char* operation, const fs::path& path,
                                      DWORD error = GetLastError()) {
    throw fs::filesystem_error(operation, path,
                               std::error_code(static_cast<int>(error), std::system_category()));
}

void close_handle(HANDLE handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
}

class UniqueHandle {
  public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        close_handle(handle_);
    }
    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            close_handle(handle_);
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept {
        return handle_;
    }
    HANDLE release() noexcept {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

  private:
    HANDLE handle_;
};

HANDLE open_root_directory(const fs::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw_windows_error("pin artifact root", path);

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information,
                                      sizeof(information))) {
        const auto error = GetLastError();
        CloseHandle(handle);
        throw_windows_error("inspect pinned artifact root", path, error);
    }
    if ((information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        throw std::runtime_error("artifact root is not a real directory: " + path.string());
    }
    return handle;
}

HANDLE open_relative(HANDLE parent, const fs::path& name, ACCESS_MASK access, ULONG share,
                     ULONG disposition, ULONG options, ULONG object_attributes,
                     DWORD& error) noexcept {
    const auto& native = name.native();
    if (native.empty() || native.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t)) {
        error = ERROR_INVALID_NAME;
        return INVALID_HANDLE_VALUE;
    }

    UNICODE_STRING unicode_name{};
    unicode_name.Buffer = const_cast<PWSTR>(native.data());
    unicode_name.Length = static_cast<USHORT>(native.size() * sizeof(wchar_t));
    unicode_name.MaximumLength = unicode_name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &unicode_name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE | object_attributes;
    IO_STATUS_BLOCK status_block{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto function = nt_create_file();
    if (function == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    const auto status = function(&handle, access, &attributes, &status_block, nullptr,
                                 FILE_ATTRIBUTE_NORMAL, share, disposition, options, nullptr, 0);
    if (status < 0) {
        error = windows_error_from_status(status);
        return INVALID_HANDLE_VALUE;
    }
    error = ERROR_SUCCESS;
    return handle;
}

HANDLE open_or_create_directory_relative(HANDLE parent, const fs::path& component,
                                         const fs::path& display_path) {
    DWORD error = ERROR_SUCCESS;
    const HANDLE handle = open_relative(
        parent, component, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT | FILE_DIRECTORY_FILE,
        object_dont_reparse, error);
    if (handle == INVALID_HANDLE_VALUE)
        throw_windows_error("open or create artifact directory", display_path, error);

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information,
                                      sizeof(information))) {
        const auto inspect_error = GetLastError();
        CloseHandle(handle);
        throw_windows_error("inspect artifact directory", display_path, inspect_error);
    }
    if ((information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        throw std::runtime_error("artifact directory or parent is not a real directory: " +
                                 display_path.string());
    }
    return handle;
}

void inspect_destination_relative(HANDLE parent, const fs::path& name,
                                  const fs::path& display_path) {
    DWORD error = ERROR_SUCCESS;
    UniqueHandle handle{
        open_relative(parent, name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, 0, error)};
    if (handle.get() == INVALID_HANDLE_VALUE) {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return;
        throw_windows_error("inspect artifact destination", display_path, error);
    }

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &information,
                                      sizeof(information))) {
        throw_windows_error("inspect artifact destination", display_path);
    }
    if ((information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error("refusing to replace reparse-point artifact: " +
                                 display_path.string());
    }
}

void mark_delete_on_close(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}

std::vector<std::byte> make_rename_information(HANDLE pinned_directory,
                                               const fs::path& destination_name) {
    const auto& native = destination_name.native();
    const auto name_bytes = native.size() * sizeof(wchar_t);
    if (name_bytes > (std::numeric_limits<DWORD>::max)() - sizeof(FILE_RENAME_INFO))
        throw std::runtime_error("artifact destination name is too long");

    std::vector<std::byte> storage(sizeof(FILE_RENAME_INFO) + name_bytes, std::byte{});
    auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    information->ReplaceIfExists = TRUE;
    // Resolve the basename against the same retained directory identity used
    // for creation. A pathname or reparse-point swap cannot redirect it.
    information->RootDirectory = pinned_directory;
    information->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(information->FileName, native.data(), name_bytes);
    return storage;
}

void rename_in_place(HANDLE file, HANDLE pinned_directory,
                     const fs::path& destination_name, const fs::path& display_path) {
    auto storage = make_rename_information(pinned_directory, destination_name);
    auto* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    const auto function = nt_set_information_file();
    if (function == nullptr)
        throw_windows_error("resolve handle-relative rename", display_path, ERROR_PROC_NOT_FOUND);
    IO_STATUS_BLOCK status_block{};
    const auto status =
        function(file, &status_block, information, static_cast<ULONG>(storage.size()),
                 static_cast<FILE_INFORMATION_CLASS>(10));
    if (status < 0)
        throw_windows_error("publish artifact", display_path, windows_error_from_status(status));
}

#else

[[noreturn]] void throw_posix_error(const char* operation, const fs::path& path,
                                    int error = errno) {
    throw fs::filesystem_error(operation, path, std::error_code(error, std::generic_category()));
}

class UniqueFd {
  public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const noexcept {
        return fd_;
    }
    int release() noexcept {
        return std::exchange(fd_, -1);
    }

  private:
    int fd_;
};

UniqueFd open_directory_at(int parent, const fs::path& component, const fs::path& display_path) {
    const auto native = component.native();
    int fd = ::openat(parent, native.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0)
        return UniqueFd{fd};
    if (errno != ENOENT)
        throw_posix_error("pin artifact directory", display_path);

    if (::mkdirat(parent, native.c_str(), 0777) != 0 && errno != EEXIST)
        throw_posix_error("create artifact directory", display_path);
    fd = ::openat(parent, native.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        throw_posix_error("pin created artifact directory", display_path);
    return UniqueFd{fd};
}

void write_all(int fd, std::span<const std::uint8_t> bytes, const fs::path& path) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto bounded = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::write(fd, bytes.data() + offset, bounded);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        throw_posix_error("write artifact", path);
    }
}

#endif

} // namespace

struct PinnedArtifactDirectory::State {
    fs::path path;
#if defined(_WIN32)
    HANDLE directory_handle{INVALID_HANDLE_VALUE};
#else
    int directory_fd{-1};
#endif

    ~State() {
#if defined(_WIN32)
        close_handle(directory_handle);
#else
        if (directory_fd >= 0)
            ::close(directory_fd);
#endif
    }
};

PinnedArtifactDirectory::PinnedArtifactDirectory(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PinnedArtifactDirectory::~PinnedArtifactDirectory() = default;
PinnedArtifactDirectory::PinnedArtifactDirectory(PinnedArtifactDirectory&&) noexcept = default;
PinnedArtifactDirectory&
PinnedArtifactDirectory::operator=(PinnedArtifactDirectory&&) noexcept = default;

PinnedArtifactDirectory PinnedArtifactDirectory::open_or_create(const fs::path& requested_path) {
    if (requested_path.empty())
        throw std::runtime_error("artifact directory must not be empty");
    const auto absolute = fs::absolute(requested_path).lexically_normal();
    auto state = std::make_unique<State>();
    state->path = absolute;

#if defined(_WIN32)
    UniqueHandle current{open_root_directory(absolute.root_path())};
    fs::path cursor = absolute.root_path();
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".")
            continue;
        cursor /= component;
        current = UniqueHandle{open_or_create_directory_relative(current.get(), component, cursor)};
    }
    state->directory_handle = current.release();
#else
    UniqueFd current{
        ::open(absolute.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (current.get() < 0)
        throw_posix_error("pin artifact root", absolute.root_path());
    fs::path cursor = absolute.root_path();
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".")
            continue;
        cursor /= component;
        current = open_directory_at(current.get(), component, cursor);
    }
    state->directory_fd = current.release();
#endif

    return PinnedArtifactDirectory{std::move(state)};
}

void PinnedArtifactDirectory::publish(std::string_view name, std::span<const std::uint8_t> bytes) {
    if (!state_)
        throw std::runtime_error("artifact directory handle is not available");
    const auto relative = checked_artifact_name(name);

#if defined(_WIN32)
    const auto destination = state_->path / relative;
    inspect_destination_relative(state_->directory_handle, relative, destination);

    fs::path temporary_name_path;
    UniqueHandle file;
    for (int attempt = 0; attempt < 64; ++attempt) {
        temporary_name_path = fs::path{temporary_name()};
        DWORD error = ERROR_SUCCESS;
        file = UniqueHandle{open_relative(state_->directory_handle, temporary_name_path,
                                          GENERIC_WRITE | DELETE | SYNCHRONIZE, 0, FILE_CREATE,
                                          FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                                              FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH,
                                          object_dont_reparse, error)};
        if (file.get() != INVALID_HANDLE_VALUE)
            break;
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            throw_windows_error("create temporary artifact", state_->path / temporary_name_path,
                                error);
    }
    if (file.get() == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot reserve a unique temporary artifact");

    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
            DWORD written = 0;
            if (!WriteFile(file.get(), bytes.data() + offset, chunk, &written, nullptr) ||
                written == 0) {
                throw_windows_error("write artifact", state_->path / temporary_name_path);
            }
            offset += written;
        }
        if (!FlushFileBuffers(file.get()))
            throw_windows_error("flush artifact", state_->path / temporary_name_path);
        rename_in_place(file.get(), state_->directory_handle, relative, destination);
    } catch (...) {
        mark_delete_on_close(file.get());
        throw;
    }
#else
    const auto native = relative.native();
    struct stat destination_status{};
    if (::fstatat(state_->directory_fd, native.c_str(), &destination_status, AT_SYMLINK_NOFOLLOW) ==
        0) {
        if (S_ISLNK(destination_status.st_mode))
            throw std::runtime_error("refusing to replace symlink artifact: " + native);
    } else if (errno != ENOENT) {
        throw_posix_error("inspect artifact destination", state_->path / relative);
    }

    std::string temporary;
    UniqueFd file;
    for (int attempt = 0; attempt < 64; ++attempt) {
        temporary = temporary_name();
        const int fd = ::openat(state_->directory_fd, temporary.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (fd >= 0) {
            file = UniqueFd{fd};
            break;
        }
        if (errno != EEXIST)
            throw_posix_error("create temporary artifact", state_->path / temporary);
    }
    if (file.get() < 0)
        throw std::runtime_error("cannot reserve a unique temporary artifact");

    struct TemporaryCleanup {
        int directory_fd;
        std::string name;
        bool active{true};
        ~TemporaryCleanup() {
            if (active)
                ::unlinkat(directory_fd, name.c_str(), 0);
        }
    } cleanup{state_->directory_fd, temporary};

    write_all(file.get(), bytes, state_->path / relative);
    if (::fsync(file.get()) != 0)
        throw_posix_error("flush artifact", state_->path / relative);
    file = UniqueFd{};
    if (::renameat(state_->directory_fd, temporary.c_str(), state_->directory_fd, native.c_str()) !=
        0) {
        throw_posix_error("publish artifact", state_->path / relative);
    }
    cleanup.active = false;
    if (::fsync(state_->directory_fd) != 0)
        throw_posix_error("flush artifact directory", state_->path);
#endif
}

} // namespace pulp::cli::gpu_artifacts
