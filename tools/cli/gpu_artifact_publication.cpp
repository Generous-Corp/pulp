#include "gpu_artifact_publication.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
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

[[noreturn]] void throw_windows_error(const char* operation, const fs::path& path,
                                      DWORD error = GetLastError()) {
    throw fs::filesystem_error(operation, path,
                               std::error_code(static_cast<int>(error), std::system_category()));
}

HANDLE open_locked_directory(const fs::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw_windows_error("pin artifact directory", path);

    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information,
                                      sizeof(information))) {
        const auto error = GetLastError();
        CloseHandle(handle);
        throw_windows_error("inspect pinned artifact directory", path, error);
    }
    if ((information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        throw std::runtime_error("artifact directory or parent is not a real directory: " +
                                 path.string());
    }
    return handle;
}

void close_handle(HANDLE handle) noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
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
    std::vector<HANDLE> directory_locks;
#else
    int directory_fd{-1};
#endif

    ~State() {
#if defined(_WIN32)
        for (auto iterator = directory_locks.rbegin(); iterator != directory_locks.rend();
             ++iterator) {
            close_handle(*iterator);
        }
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
    fs::path cursor = absolute.root_path();
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".")
            continue;
        cursor /= component;
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw_windows_error("inspect artifact directory", cursor, error);
            if (!CreateDirectoryW(cursor.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                throw_windows_error("create artifact directory", cursor);
            }
        }
        state->directory_locks.push_back(open_locked_directory(cursor));
    }
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
    const auto attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error("refusing to replace reparse-point artifact: " +
                                 destination.string());
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            throw_windows_error("inspect artifact destination", destination, error);
    }

    fs::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 64; ++attempt) {
        temporary = state_->path / fs::path{temporary_name()};
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                               FILE_FLAG_WRITE_THROUGH,
                           nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            throw_windows_error("create temporary artifact", temporary);
    }
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot reserve a unique temporary artifact");

    bool published = false;
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
                throw_windows_error("write artifact", temporary);
            }
            offset += written;
        }
        if (!FlushFileBuffers(file))
            throw_windows_error("flush artifact", temporary);
        if (!CloseHandle(file))
            throw_windows_error("close artifact", temporary);
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw_windows_error("publish artifact", destination);
        }
        published = true;
    } catch (...) {
        close_handle(file);
        if (!published)
            DeleteFileW(temporary.c_str());
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
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
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
