// widget_bridge/private_storage.hpp - owner-only, per-user, per-bridge file
// storage for the bridge's persisted key-value and style-preset stores.
//
// Script-supplied names never reach the filesystem verbatim: the key-value
// store hashes every key, and the preset store whitelists the name. Both live
// under the per-user settings root (never the world-shared temp dir), in a
// subdirectory keyed by the bridge's script base dir so two plugins cannot
// read or clobber each other's entries. The directory is resolved once per
// bridge and script base dir (WidgetBridge::PrivateStoreDirs), so a UI that
// mirrors every control change into storage pays one hash and one file write
// per event, never a directory walk. Writes land in an exclusively created
// sibling that is renamed over the entry without an fsync - the store holds
// UI preferences, not documents - and reads refuse anything that is not a
// plain regular file.
#pragma once

#include "../user_data_paths.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::view::detail {

enum class PrivateStoreKind { WidgetStorage, StylePresets };

inline const char* private_store_kind_dir(PrivateStoreKind kind) {
    return kind == PrivateStoreKind::WidgetStorage ? "widget-storage" : "style-presets";
}

// Create one directory level owner-only. Permissions are set only when this
// call created the directory, so a pre-existing tree keeps whatever the user
// gave it; a filesystem without POSIX bits ignores the request.
inline bool create_owner_only_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    const bool created = std::filesystem::create_directory(dir, ec);
    if (ec)
        return false;
    if (created) {
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
    }
    return true;
}

// Resolve <settings root>/Pulp/<kind>/<namespace> for one bridge, creating the
// <kind> and <namespace> levels owner-only. The shared "Pulp" root is also
// home to content packs and imported assets, so it is created with default
// permissions and never re-permissioned here. The namespace is a short hash
// of the bridge's storage identity (its script base dir, or the identity an
// embedded editor's host names); a bridge without one shares a single
// "shared" namespace. Returns an empty path when no per-user root is known,
// so callers fail closed rather than fall back to a shared location.
inline std::filesystem::path resolve_private_dir(PrivateStoreKind kind,
                                                 const std::string& storage_identity) {
    const auto root = platform_settings_root();
    if (root.empty())
        return {};

    std::string ns = "shared";
    if (!storage_identity.empty())
        ns = pulp::runtime::sha256_hex(storage_identity).substr(0, 16);

    std::error_code ec;
    const auto pulp_dir = root / "Pulp";
    std::filesystem::create_directories(pulp_dir, ec);
    if (ec)
        return {};
    const auto kind_dir = pulp_dir / private_store_kind_dir(kind);
    if (!create_owner_only_dir(kind_dir))
        return {};
    const auto dir = kind_dir / ns;
    if (!create_owner_only_dir(dir))
        return {};
    return dir;
}

// The bridge-cached directory for `kind`, resolved on first use and again
// only when the storage identity has changed since. `Cache` is the bridge's
// private PrivateStoreDirs, reached through the registrars' friend access.
template <class Cache>
const std::filesystem::path& cached_private_dir(Cache& cache, PrivateStoreKind kind,
                                                const std::string& storage_identity) {
    if (cache.identity != storage_identity) {
        cache.identity = storage_identity;
        cache.widget_storage.reset();
        cache.style_presets.reset();
    }
    auto& slot =
        kind == PrivateStoreKind::WidgetStorage ? cache.widget_storage : cache.style_presets;
    if (!slot)
        slot = resolve_private_dir(kind, storage_identity);
    return *slot;
}

// Key-value entries are stored under the SHA-256 of the key: any key works,
// including ones carrying "/", "..", ":" or Windows-reserved names, and none
// of them can name a path outside the store.
inline std::string storage_key_file_name(std::string_view key) {
    return pulp::runtime::sha256_hex(key) + ".dat";
}

// Open `path` for writing only if this call creates it; a symlink at the
// path is never followed. Returns a descriptor or -1.
inline int open_exclusive_for_write(const std::filesystem::path& path) {
#if defined(_WIN32)
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
                  _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0)
        return -1;
    return fd;
#else
    return ::open(path.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0600);
#endif
}

inline bool write_fully(int fd, std::string_view bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto chunk = static_cast<unsigned int>(
            std::min<std::size_t>(bytes.size() - done, static_cast<std::size_t>(1) << 30));
#if defined(_WIN32)
        const auto n = _write(fd, bytes.data() + done, chunk);
#else
        const auto n = ::write(fd, bytes.data() + done, chunk);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n <= 0)
            return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

inline void close_fd(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    ::close(fd);
#endif
}

inline int current_pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

// Write `bytes` to `destination` through an exclusively created sibling that
// is renamed over the entry. A destination that already exists as anything
// but a plain regular file (a symlink, a directory) is refused. No fsync:
// durability of a UI preference is not worth a flush per control change.
inline bool write_private_file(const std::filesystem::path& destination, std::string_view bytes) {
    std::error_code ec;
    const auto existing = std::filesystem::symlink_status(destination, ec);
    if (ec && existing.type() != std::filesystem::file_type::not_found)
        return false;
    if (std::filesystem::exists(existing) && !std::filesystem::is_regular_file(existing))
        return false;

    static std::atomic<std::uint64_t> serial{0};
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto temporary = destination;
        temporary +=
            ".tmp-" + std::to_string(current_pid()) + "-" + std::to_string(serial.fetch_add(1));
        const int fd = open_exclusive_for_write(temporary);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return false;
        }
        const bool written = write_fully(fd, bytes);
        close_fd(fd);
        if (written) {
            std::filesystem::rename(temporary, destination, ec);
            if (!ec)
                return true;
        }
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return false;
}

// Read `path` only when it is a plain regular file (not a symlink, not a
// directory); otherwise nullopt.
inline std::optional<std::string> read_private_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status))
        return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace pulp::view::detail
