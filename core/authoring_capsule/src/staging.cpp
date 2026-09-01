#include <pulp/authoring_capsule/staging.hpp>

#include <pulp/authoring_capsule/safe_path.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace pulp::authoring_capsule {
namespace fs = std::filesystem;

namespace {

// ── Small conversions ───────────────────────────────────────────────────

#if defined(_WIN32)
/// Manifest paths are UTF-8 by definition, so they are widened explicitly
/// rather than handed to `fs::path`'s narrow constructor, which interprets
/// bytes in the active code page and would mangle a non-ASCII member name.
fs::path path_from_utf8(std::string_view value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
#endif

std::string path_to_utf8(const fs::path& value) {
    const auto encoded = value.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

runtime::Result<void, CapsuleError> fail(CapsuleStatus status, std::string subject,
                                         std::string required = {}, std::string found = {}) {
    return runtime::Result<void, CapsuleError>(runtime::Err(
        CapsuleError{status, std::move(subject), std::move(required), std::move(found)}));
}

// ── Digest comparison ───────────────────────────────────────────────────

/// Compare a declared digest against a computed one. Hex is compared without
/// regard to ASCII case, and a `sha256:` prefix is tolerated on the declared
/// side because the manifest spells a bare digest on a file row and a prefixed
/// one on a revision identity. Nothing else is accepted: a declared digest that
/// is not the same 32 bytes never matches.
bool digest_equal(std::string_view declared, std::string_view computed) noexcept {
    constexpr std::string_view prefix = "sha256:";
    if (declared.size() > prefix.size() && declared.substr(0, prefix.size()) == prefix)
        declared.remove_prefix(prefix.size());
    if (declared.size() != computed.size())
        return false;
    for (std::size_t index = 0; index < declared.size(); ++index) {
        auto lhs = static_cast<unsigned char>(declared[index]);
        const auto rhs = static_cast<unsigned char>(computed[index]);
        if (lhs >= 'A' && lhs <= 'F')
            lhs = static_cast<unsigned char>(lhs - 'A' + 'a');
        if (lhs != rhs)
            return false;
    }
    return true;
}

// ── Path containment ────────────────────────────────────────────────────

/// Split an already-normalized member path into components, refusing anything
/// that could resolve outside the staging root.
///
/// `admit_member_path` runs first and is the authority for the full path
/// grammar — NFC, byte and depth budgets, confusables, reserved names. This
/// repeats only the containment subset, deliberately: extraction is the last
/// point before an untrusted string is joined to a real directory, and a
/// containment bug there writes outside the staging area. Two independent
/// checks of the property that matters most is worth the duplication.
bool split_contained_path(std::string_view path, std::vector<std::string>& components) {
    components.clear();
    if (path.empty() || path.front() == '/' || path.back() == '/')
        return false;
    std::size_t start = 0;
    for (;;) {
        const auto separator = path.find('/', start);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        const auto component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        for (const char byte : component) {
            const auto value = static_cast<unsigned char>(byte);
            if (value < 0x20 || value == 0x7f || byte == '\\')
                return false;
#if defined(_WIN32)
            // A colon opens an alternate data stream, and no Windows filename
            // may legitimately contain one.
            if (byte == ':')
                return false;
#endif
        }
        components.emplace_back(component);
        if (separator == std::string_view::npos)
            break;
        start = separator + 1;
    }
    return !components.empty();
}

// ── Native directory handle ─────────────────────────────────────────────

constexpr std::intptr_t kInvalidNative = -1;

/// A directory pinned by handle. Holding the handle is what lets the staging
/// area prove that a pathname still denotes the directory it created, rather
/// than something that rebound to the name in between.
class DirectoryHandle {
  public:
    DirectoryHandle() noexcept = default;
    ~DirectoryHandle() { close(); }
    DirectoryHandle(DirectoryHandle&& other) noexcept
        : native_(std::exchange(other.native_, kInvalidNative)) {}
    DirectoryHandle& operator=(DirectoryHandle&& other) noexcept {
        if (this != &other) {
            close();
            native_ = std::exchange(other.native_, kInvalidNative);
        }
        return *this;
    }
    DirectoryHandle(const DirectoryHandle&) = delete;
    DirectoryHandle& operator=(const DirectoryHandle&) = delete;

    static DirectoryHandle open(const fs::path& path) noexcept {
#if defined(_WIN32)
        // FILE_SHARE_DELETE keeps this handle from vetoing the publication
        // rename of the directory it pins.
        const auto handle = ::CreateFileW(
            path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return {};
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileInformationByHandle(handle, &info) == 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ::CloseHandle(handle);
            return {};
        }
        return DirectoryHandle(reinterpret_cast<std::intptr_t>(handle));
#else
        const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                         | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                                         | O_NOFOLLOW
#endif
        );
        if (descriptor < 0)
            return {};
        return DirectoryHandle(static_cast<std::intptr_t>(descriptor));
#endif
    }

    bool valid() const noexcept { return native_ != kInvalidNative; }

#if !defined(_WIN32)
    int descriptor() const noexcept { return static_cast<int>(native_); }
#endif

    /// True only while `path` still names this exact directory.
    bool still_named_by(const fs::path& path) const noexcept {
        if (!valid())
            return false;
#if defined(_WIN32)
        const auto named =
            ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                          nullptr);
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
        struct stat pinned{};
        struct stat named{};
        return ::fstat(static_cast<int>(native_), &pinned) == 0 &&
               ::lstat(path.c_str(), &named) == 0 && S_ISDIR(named.st_mode) &&
               pinned.st_dev == named.st_dev && pinned.st_ino == named.st_ino;
#endif
    }

    void close() noexcept {
        if (!valid())
            return;
#if defined(_WIN32)
        ::CloseHandle(reinterpret_cast<HANDLE>(native_));
#else
        ::close(static_cast<int>(native_));
#endif
        native_ = kInvalidNative;
    }

  private:
    explicit DirectoryHandle(std::intptr_t native) noexcept : native_(native) {}
    std::intptr_t native_ = kInvalidNative;
};

// ── Durability fences ───────────────────────────────────────────────────

#if !defined(_WIN32)
bool fence_descriptor(int descriptor) noexcept {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    // fsync() on Apple platforms only pushes as far as the drive's write cache;
    // F_FULLFSYNC is what actually orders the write against a power loss.
    if (::fcntl(descriptor, F_FULLFSYNC) == 0)
        return true;
    if (errno != ENOTSUP)
        return false;
#endif
    return ::fsync(descriptor) == 0;
}
#endif

bool fence_file(const fs::path& path) noexcept {
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
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
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

bool fence_directory(const fs::path& path) noexcept {
#if defined(_WIN32)
    // Windows exposes write-through rename as its namespace fence; a directory
    // handle cannot portably be flushed.
    (void)path;
    return true;
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
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

// ── Owner-private directory creation ────────────────────────────────────

enum class PrivateDirectory : std::uint8_t { created, already_exists, failed };

#if defined(_WIN32)
/// Protected DACL granting only SYSTEM, the local administrators group, and the
/// owner, inherited by everything created beneath it. `P` blocks inheritance
/// from the enclosing directory, so a permissive ACE upstream cannot leak the
/// capsule being staged.
constexpr const wchar_t* kPrivateDirectorySddl =
    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)";
#endif

PrivateDirectory create_private_directory(const fs::path& path) noexcept {
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kPrivateDirectorySddl, SDDL_REVISION_1, &descriptor, nullptr) == 0)
        return PrivateDirectory::failed;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const bool created = ::CreateDirectoryW(path.c_str(), &attributes) != 0;
    const auto error = created ? ERROR_SUCCESS : ::GetLastError();
    ::LocalFree(descriptor);
    if (created)
        return PrivateDirectory::created;
    return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
               ? PrivateDirectory::already_exists
               : PrivateDirectory::failed;
#else
    if (::mkdir(path.c_str(), 0700) != 0)
        return errno == EEXIST ? PrivateDirectory::already_exists : PrivateDirectory::failed;
    // mkdir's mode argument is filtered by the umask and a setgid parent adds
    // bits of its own, so the resulting mode is re-read and corrected rather
    // than assumed. A directory that cannot be proven owner-only is removed
    // instead of used: staging that is not private is not staging.
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                     | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                                     | O_NOFOLLOW
#endif
    );
    if (descriptor >= 0) {
        struct stat status{};
        bool owner_only = ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode);
        if (owner_only && (status.st_mode & 07777) != 0700)
            owner_only = ::fchmod(descriptor, static_cast<mode_t>(0700)) == 0 &&
                         ::fstat(descriptor, &status) == 0;
        owner_only = owner_only && (status.st_mode & 07777) == 0700 && status.st_uid == ::geteuid();
        ::close(descriptor);
        if (owner_only)
            return PrivateDirectory::created;
    }
    std::error_code ignored;
    fs::remove(path, ignored);
    return PrivateDirectory::failed;
#endif
}

fs::path staging_candidate(const fs::path& parent) {
    static std::atomic<std::uint64_t> serial{0};
    const auto tick =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
    const auto process = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    const auto process = static_cast<unsigned long long>(::getpid());
#endif
    std::array<char, 96> name{};
    std::snprintf(name.data(), name.size(), ".pulp-capsule-staging-%llu-%llu", process,
                  static_cast<unsigned long long>(tick ^ serial.fetch_add(1)));
    return parent / name.data();
}

// ── No-replace publication ──────────────────────────────────────────────

enum class Publication : std::uint8_t { published, destination_exists, failed };

/// Move a staged directory onto a name that must be free.
///
/// Plain `rename()` replaces an existing empty destination directory, so it is
/// never used alone: a race with a second importer would resolve to a clobber
/// instead of a refusal. The kernel's exclusive-rename primitive is tried
/// first. Where it is unavailable the fallback reserves the destination name
/// with an atomic `mkdir` and then renames onto that reservation, so the only
/// directory `rename()` can consume is the empty one this call just created.
Publication publish_directory_no_replace(const fs::path& source,
                                         const fs::path& destination) noexcept {
#if defined(_WIN32)
    // Without MOVEFILE_REPLACE_EXISTING, MoveFileEx fails rather than replaces.
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
        return Publication::published;
    const auto error = ::GetLastError();
    return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
               ? Publication::destination_exists
               : Publication::failed;
#else
    bool primitive_unavailable = true;
#if defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0)
        return Publication::published;
    if (errno == EEXIST)
        return Publication::destination_exists;
    primitive_unavailable = errno == ENOTSUP || errno == ENOSYS || errno == EINVAL;
#elif defined(__linux__)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) == 0)
        return Publication::published;
    if (errno == EEXIST)
        return Publication::destination_exists;
    primitive_unavailable = errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP;
#endif
    if (!primitive_unavailable)
        return Publication::failed;

    if (::mkdir(destination.c_str(), 0700) != 0)
        return errno == EEXIST ? Publication::destination_exists : Publication::failed;
    if (::rename(source.c_str(), destination.c_str()) == 0)
        return Publication::published;
    const auto rename_error = errno;
    // Removing the reservation restores the destination namespace exactly as it
    // was found. rmdir succeeds only while the reservation is still empty,
    // which is precisely the case where nothing else has claimed the name.
    ::rmdir(destination.c_str());
    return rename_error == ENOTEMPTY || rename_error == EEXIST ? Publication::destination_exists
                                                               : Publication::failed;
#endif
}

// ── Member writing ──────────────────────────────────────────────────────

enum class MemberWrite : std::uint8_t { ok, collides, failed };

#if defined(_WIN32)
bool write_all(HANDLE handle, std::span<const std::uint8_t> bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<std::size_t>(0x7ffff000u)));
        DWORD written = 0;
        if (::WriteFile(handle, bytes.data() + offset, request, &written, nullptr) == 0 ||
            written == 0)
            return false;
        offset += written;
    }
    return true;
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
#endif

/// Create every intermediate directory and then the member file itself,
/// exclusively. The file is created with `O_EXCL` / `CREATE_NEW`, so a second
/// declaration of the same path is reported as a collision rather than
/// overwriting the member that was already verified and written.
MemberWrite write_member(const DirectoryHandle& root, const fs::path& root_path,
                         const std::vector<std::string>& components,
                         std::span<const std::uint8_t> bytes) noexcept {
#if defined(_WIN32)
    (void)root;
    fs::path current = root_path;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        current /= path_from_utf8(components[index]);
        if (::CreateDirectoryW(current.c_str(), nullptr) != 0)
            continue;
        if (::GetLastError() != ERROR_ALREADY_EXISTS)
            return MemberWrite::failed;
        const auto attributes = ::GetFileAttributesW(current.c_str());
        // A declared path that needs a directory where a member file already
        // sits is a collision, not an I/O fault.
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return MemberWrite::collides;
    }
    current /= path_from_utf8(components.back());
    const auto handle =
        ::CreateFileW(current.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? MemberWrite::collides
                                                                           : MemberWrite::failed;
    }
    const bool written = write_all(handle, bytes);
    const bool closed = ::CloseHandle(handle) != 0;
    return written && closed ? MemberWrite::ok : MemberWrite::failed;
#else
    (void)root_path;
    // Every hop is opened relative to the pinned root with O_NOFOLLOW, so no
    // component of the path can redirect the write through a symlink.
    const int root_descriptor = root.descriptor();
    int current = root_descriptor;
    const auto release = [root_descriptor](int descriptor) noexcept {
        if (descriptor != root_descriptor && descriptor >= 0)
            ::close(descriptor);
    };
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const auto& component = components[index];
        if (::mkdirat(current, component.c_str(), 0700) != 0 && errno != EEXIST) {
            release(current);
            return MemberWrite::failed;
        }
        const auto next = ::openat(current, component.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                                   | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                                                   | O_NOFOLLOW
#endif
        );
        const auto open_error = errno;
        release(current);
        if (next < 0)
            // A declared path that needs a directory where a member file
            // already sits is a collision, not an I/O fault. ELOOP is not: with
            // O_NOFOLLOW it means the name is a symlink, which nothing here
            // creates, so the staged tree is not what this module built.
            return open_error == ENOTDIR ? MemberWrite::collides : MemberWrite::failed;
        current = next;
    }
    const auto descriptor = ::openat(current, components.back().c_str(),
                                     O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                         | O_NOFOLLOW
#endif
                                     ,
                                     0600);
    const auto create_error = errno;
    release(current);
    if (descriptor < 0)
        // O_CREAT | O_EXCL reports every pre-existing name as EEXIST whatever
        // its type, so that is the whole collision case.
        return create_error == EEXIST ? MemberWrite::collides : MemberWrite::failed;
    const bool written = write_all(descriptor, bytes);
    const bool closed = ::close(descriptor) == 0;
    return written && closed ? MemberWrite::ok : MemberWrite::failed;
#endif
}

// ── Member reading ──────────────────────────────────────────────────────

enum class MemberRead : std::uint8_t { ok, size_mismatch, failed };

/// Read one staged member, walking the path components relative to the pinned
/// root and refusing to follow a link at any hop, exactly as writing one does.
///
/// The size on disk is compared against the row's declared size *before* the
/// bytes are copied out, so the allocation is bounded by what the owner-private
/// tree actually holds rather than by a number a caller-supplied row asserted.
/// A disagreement is reported rather than repaired: a member that is not the
/// length its row declares is not the member that was verified as it landed.
MemberRead read_member(const DirectoryHandle& root, const fs::path& root_path,
                       const std::vector<std::string>& components, std::uint64_t declared_bytes,
                       std::vector<std::uint8_t>& bytes, std::uint64_t& found_bytes) {
    bytes.clear();
    found_bytes = 0;

    const auto materialize = [&bytes](std::uint64_t size) {
        if (size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
            return false;
        try {
            bytes.resize(static_cast<std::size_t>(size));
        } catch (const std::bad_alloc&) {
            // A member the machine cannot hold is a staging failure, not a
            // silent short read: returning fewer bytes than the row declares
            // would hand the caller a truncated file that looks complete.
            return false;
        }
        return true;
    };

#if defined(_WIN32)
    (void)root;
    fs::path current = root_path;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        current /= path_from_utf8(components[index]);
        const auto attributes = ::GetFileAttributesW(current.c_str());
        // Nothing in this module creates a reparse point, so one standing on
        // the path means the staged tree is not what this module built.
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return MemberRead::failed;
    }
    current /= path_from_utf8(components.back());
    const auto handle = ::CreateFileW(current.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return MemberRead::failed;
    const auto finish = [handle](MemberRead outcome) noexcept {
        ::CloseHandle(handle);
        return outcome;
    };

    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(handle, &info) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return finish(MemberRead::failed);

    found_bytes = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
                  static_cast<std::uint64_t>(info.nFileSizeLow);
    if (found_bytes != declared_bytes)
        return finish(MemberRead::size_mismatch);
    if (!materialize(found_bytes))
        return finish(MemberRead::failed);

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset, static_cast<std::size_t>(0x7ffff000u)));
        DWORD read = 0;
        if (::ReadFile(handle, bytes.data() + offset, request, &read, nullptr) == 0 || read == 0)
            return finish(MemberRead::failed);
        offset += read;
    }
    return finish(MemberRead::ok);
#else
    (void)root_path;
    // Every hop is opened relative to the pinned root with O_NOFOLLOW, so no
    // component of the path can redirect the read through a symlink.
    const int root_descriptor = root.descriptor();
    int current = root_descriptor;
    const auto release = [root_descriptor](int descriptor) noexcept {
        if (descriptor != root_descriptor && descriptor >= 0)
            ::close(descriptor);
    };
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const auto next = ::openat(current, components[index].c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                                          | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                                                          | O_NOFOLLOW
#endif
        );
        release(current);
        if (next < 0)
            return MemberRead::failed;
        current = next;
    }
    const auto descriptor = ::openat(current, components.back().c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                                                             | O_NOFOLLOW
#endif
    );
    release(current);
    if (descriptor < 0)
        return MemberRead::failed;
    const auto finish = [descriptor](MemberRead outcome) noexcept {
        ::close(descriptor);
        return outcome;
    };

    struct stat status{};
    // Only a plain file. A directory, a device, or a fifo standing where a
    // member belongs means the tree is not the one extraction wrote.
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode))
        return finish(MemberRead::failed);

    found_bytes = static_cast<std::uint64_t>(status.st_size);
    if (found_bytes != declared_bytes)
        return finish(MemberRead::size_mismatch);
    if (!materialize(found_bytes))
        return finish(MemberRead::failed);

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        // A short read before the declared end means the file changed under
        // the reader; the partial buffer is discarded rather than returned.
        if (count <= 0)
            return finish(MemberRead::failed);
        offset += static_cast<std::size_t>(count);
    }
    return finish(MemberRead::ok);
#endif
}

}  // namespace

// ── StagingArea ─────────────────────────────────────────────────────────

struct StagingArea::Impl {
    fs::path root;
    DirectoryHandle handle;
    bool published = false;

    /// Remove the staged tree unless it was published. Deleting by pathname is
    /// only safe while the pathname still denotes the directory this object
    /// created; when it does not, leaking an unreachable private directory is
    /// far better than recursively removing whatever rebound to the name.
    void discard() noexcept {
        if (published || root.empty()) {
            handle.close();
            return;
        }
        const bool ours = handle.still_named_by(root);
        handle.close();
        if (ours) {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
        root.clear();
    }
};

StagingArea::StagingArea(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StagingArea::StagingArea(StagingArea&&) noexcept = default;

StagingArea& StagingArea::operator=(StagingArea&& other) noexcept {
    if (this != &other) {
        if (impl_)
            impl_->discard();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

StagingArea::~StagingArea() {
    if (impl_)
        impl_->discard();
}

const fs::path& StagingArea::root() const noexcept {
    static const fs::path empty;
    return impl_ ? impl_->root : empty;
}

runtime::Result<StagingArea, CapsuleError> StagingArea::create(const fs::path& parent) {
    const auto reject = [](const fs::path& subject) {
        return runtime::Result<StagingArea, CapsuleError>(runtime::Err(
            CapsuleError{CapsuleStatus::staging_failed, path_to_utf8(subject), {}, {}}));
    };
    if (parent.empty())
        return reject(parent);

    // Resolving the parent up front means the pathname the caller sees and the
    // pathname the identity checks use are the same one.
    std::error_code error;
    const auto resolved = fs::canonical(parent, error);
    if (error)
        return reject(parent);
    if (!fs::is_directory(resolved, error) || error)
        return reject(parent);

    // A name collision is a retry, not a failure: the candidate carries a clock
    // and a counter, so a second attempt lands elsewhere.
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
        const auto candidate = staging_candidate(resolved);
        const auto created = create_private_directory(candidate);
        if (created == PrivateDirectory::already_exists)
            continue;
        if (created == PrivateDirectory::failed)
            return reject(candidate);
        auto handle = DirectoryHandle::open(candidate);
        if (!handle.valid() || !handle.still_named_by(candidate)) {
            handle.close();
            std::error_code ignored;
            fs::remove_all(candidate, ignored);
            return reject(candidate);
        }
        auto impl = std::make_unique<Impl>();
        impl->root = candidate;
        impl->handle = std::move(handle);
        return runtime::Result<StagingArea, CapsuleError>(runtime::Ok(StagingArea(std::move(impl))));
    }
    return reject(resolved);
}

runtime::Result<void, CapsuleError> StagingArea::publish_no_replace(const fs::path& destination) {
    if (!impl_ || impl_->published || impl_->root.empty() || !impl_->handle.valid() ||
        destination.empty())
        return fail(CapsuleStatus::staging_failed, path_to_utf8(destination));

    std::error_code error;
    const auto absolute_destination = fs::absolute(destination, error);
    if (error)
        return fail(CapsuleStatus::staging_failed, path_to_utf8(destination));
    // A trailing separator makes `parent_path()` name the destination itself,
    // which would publish somewhere the caller did not ask for. A destination
    // must name a leaf.
    if (absolute_destination.filename().empty())
        return fail(CapsuleStatus::staging_failed, path_to_utf8(destination));
    const auto destination_parent = absolute_destination.parent_path();
    // The destination's parent is not created here. Publication places a tree
    // under a directory the consumer already chose; inventing that directory
    // would be a decision this layer does not own.
    if (destination_parent.empty() || !fs::is_directory(destination_parent, error) || error)
        return fail(CapsuleStatus::staging_failed, path_to_utf8(destination_parent));

    // There is deliberately no early "is the name taken" check. It would be
    // cheaper on the failure path, but it is also the check a test can observe:
    // with one in place, breaking the exclusive rename below leaves every test
    // passing, so the guard that actually decides a race would be the untested
    // one. The rename is the only authority, and it is the one exercised.
    error.clear();

    if (!impl_->handle.still_named_by(impl_->root))
        return fail(CapsuleStatus::staging_failed, path_to_utf8(impl_->root));

    // Walk the staged tree once, refusing anything that is not a plain file or
    // directory. Nothing in this module creates a symlink, a device, or a
    // hardlinked member, so one appearing here is evidence the tree is not what
    // this object built.
    std::vector<fs::path> directories{impl_->root};
    std::vector<fs::path> files;
    error.clear();
    for (fs::recursive_directory_iterator iterator(impl_->root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error)
            break;
        if (status.type() == fs::file_type::directory)
            directories.push_back(iterator->path());
        else if (status.type() == fs::file_type::regular)
            files.push_back(iterator->path());
        else
            return fail(CapsuleStatus::staging_failed, path_to_utf8(iterator->path()));
    }
    if (error)
        return fail(CapsuleStatus::staging_failed, path_to_utf8(impl_->root));

    // Fence ordering: file contents first, then the directories that name them
    // deepest-first, then the rename, then the destination's parent. Every name
    // the published tree depends on is durable before that tree is reachable,
    // so a crash can leave the destination absent but never half-populated.
    for (const auto& file : files)
        if (!fence_file(file))
            return fail(CapsuleStatus::staging_failed, path_to_utf8(file));
    std::sort(directories.begin(), directories.end(), [](const auto& lhs, const auto& rhs) {
        return std::distance(lhs.begin(), lhs.end()) > std::distance(rhs.begin(), rhs.end());
    });
    for (const auto& directory : directories)
        if (!fence_directory(directory))
            return fail(CapsuleStatus::staging_failed, path_to_utf8(directory));

    if (!impl_->handle.still_named_by(impl_->root))
        return fail(CapsuleStatus::staging_failed, path_to_utf8(impl_->root));

#if defined(_WIN32)
    // Windows renames the directory by name, and an open handle is one more way
    // for that to be refused. The handle is reopened when publication does not
    // happen, so a refusal still leaves a staging area that can clean itself up.
    impl_->handle.close();
#endif
    const auto publication = publish_directory_no_replace(impl_->root, absolute_destination);
    if (publication != Publication::published) {
#if defined(_WIN32)
        impl_->handle = DirectoryHandle::open(impl_->root);
#endif
        return publication == Publication::destination_exists
                   ? fail(CapsuleStatus::publication_conflict, path_to_utf8(absolute_destination))
                   : fail(CapsuleStatus::staging_failed, path_to_utf8(absolute_destination));
    }

    // The tree is visible under its final name, so this object no longer owns
    // anything to remove. Marking it published before the closing fence is what
    // keeps a fence failure from deleting a published project.
    impl_->published = true;
    impl_->handle.close();

    // Best effort by design. The publication has already happened and is
    // visible; reporting a failure here would tell the caller the tree was not
    // published while it plainly was, and a retry would then hit a conflict
    // against this very tree.
    fence_directory(destination_parent);
    return {};
}

// ── Extraction ──────────────────────────────────────────────────────────

runtime::Result<void, CapsuleError> extract_declared(const CapsuleArchive& archive,
                                                     const Manifest& manifest,
                                                     const StagingArea& staging,
                                                     const ExtractionProgress& progress) {
    const auto& root = staging.root();
    if (root.empty())
        return fail(CapsuleStatus::staging_failed, {});
    auto root_handle = DirectoryHandle::open(root);
    if (!root_handle.valid() || !root_handle.still_named_by(root))
        return fail(CapsuleStatus::staging_failed, path_to_utf8(root));

    // Iterating the manifest — never the archive — is what keeps an undeclared
    // member from being written: a capsule cannot smuggle a file past the
    // closure it published. Membership is still checked so a declared-but-absent
    // row is named as the closure violation it is, rather than surfacing as an
    // opaque read failure.
    const auto members = archive.members();
    std::vector<std::string_view> member_paths;
    member_paths.reserve(members.size());
    for (const auto& member : members)
        member_paths.emplace_back(member.path);
    std::sort(member_paths.begin(), member_paths.end());

    std::vector<std::string> components;
    const auto count = manifest.files.size();
    for (std::size_t index = 0; index < count; ++index) {
        const FileEntry& entry = manifest.files[index];

        // Cancellation is checked before the member is read, so a cancelled
        // import has neither expanded nor written the member it stopped on.
        if (progress && !progress(index, count))
            return fail(CapsuleStatus::cancelled, entry.path);

        // The full grammar, not just containment. This is a public entry point
        // taking an arbitrary Manifest, so it cannot assume a caller ran
        // preview first: a path that never faced the byte, depth, NFC, or
        // reserved-name rules would otherwise reach the filesystem through a
        // caller that skipped admission.
        auto admitted = admit_member_path(entry.path);
        if (!admitted) return runtime::Err(std::move(admitted).error());
        if (!split_contained_path(entry.path, components))
            return fail(CapsuleStatus::path_rejected, entry.path);
        if (!std::binary_search(member_paths.begin(), member_paths.end(),
                                std::string_view(entry.path)))
            return fail(CapsuleStatus::closure_violation, entry.path);

        auto expanded = archive.read(entry.path);
        if (expanded.is_err())
            return runtime::Result<void, CapsuleError>(runtime::Err(std::move(expanded).error()));

        // Verified before a byte reaches the filesystem, so a member whose
        // content disagrees with its declaration is never written at all and a
        // refused import cannot leave a plausible-looking wrong file behind.
        const auto computed = runtime::sha256_hex(expanded->data(), expanded->size());
        if (!digest_equal(entry.sha256, computed))
            return fail(CapsuleStatus::digest_mismatch, entry.path, entry.sha256, computed);

        if (!root_handle.still_named_by(root))
            return fail(CapsuleStatus::staging_failed, path_to_utf8(root));

        const auto written =
            write_member(root_handle, root, components,
                         std::span<const std::uint8_t>(expanded->data(), expanded->size()));
        if (written == MemberWrite::collides)
            return fail(CapsuleStatus::path_collision, entry.path);
        if (written != MemberWrite::ok)
            return fail(CapsuleStatus::staging_failed, entry.path);
    }
    return {};
}

runtime::Result<std::vector<std::uint8_t>, CapsuleError>
read_staged_member(const std::filesystem::path& staging_root, const FileEntry& entry) {
    using Result = runtime::Result<std::vector<std::uint8_t>, CapsuleError>;
    const auto reject = [](CapsuleStatus status, std::string subject, std::string required = {},
                           std::string found = {}) {
        return Result(runtime::Err(
            CapsuleError{status, std::move(subject), std::move(required), std::move(found)}));
    };

    const auto& root = staging_root;
    if (root.empty())
        return reject(CapsuleStatus::staging_failed, {});
    auto root_handle = DirectoryHandle::open(root);
    if (!root_handle.valid() || !root_handle.still_named_by(root))
        return reject(CapsuleStatus::staging_failed, path_to_utf8(root));

    // The row's path faces the full grammar again, for the same reason
    // extraction re-admits it: this is a public entry point taking an arbitrary
    // FileEntry, so it cannot assume a caller ran preview or extraction first.
    // The join happens here, inside the layer that owns the admission rules,
    // rather than in every consumer that holds a staging root.
    auto admitted = admit_member_path(entry.path);
    if (!admitted) return Result(runtime::Err(std::move(admitted).error()));
    const std::string path = std::move(admitted).value();

    std::vector<std::string> components;
    if (!split_contained_path(path, components))
        return reject(CapsuleStatus::path_rejected, path);

    std::vector<std::uint8_t> bytes;
    std::uint64_t found_bytes = 0;
    const auto outcome =
        read_member(root_handle, root, components, entry.bytes, bytes, found_bytes);
    if (outcome == MemberRead::size_mismatch)
        return reject(CapsuleStatus::staging_failed, path, std::to_string(entry.bytes),
                      std::to_string(found_bytes));
    if (outcome != MemberRead::ok)
        return reject(CapsuleStatus::staging_failed, path);

    if (!root_handle.still_named_by(root))
        return reject(CapsuleStatus::staging_failed, path_to_utf8(root));

    return Result(runtime::Ok(std::move(bytes)));
}

runtime::Result<std::vector<std::uint8_t>, CapsuleError>
read_staged_member(const StagingArea& staging, const FileEntry& entry) {
    return read_staged_member(staging.root(), entry);
}

}  // namespace pulp::authoring_capsule
