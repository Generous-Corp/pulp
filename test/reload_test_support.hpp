#pragma once

// Per-process-unique temp paths for the reload test suites.
//
// ctest runs each Catch2 TEST_CASE as its own process, so a `static int counter`
// restarts at 1 in every case — meaning sibling cases (and other suites) compute
// the SAME `temp_directory_path()/prefixN` path. Under parallel ctest that races:
// one case's remove_all() deletes another's fixture files mid-test, which surfaces
// as empty reads / signature failures that only reproduce under load (not in a
// serial local run). Keying the path on the PID as well makes it unique per
// process, so parallel cases never share a directory.
//
// Every path handed out is removed when the test process exits, including
// trees a test locked read-only: an installed swap pack is published
// `dr-x------` with `r--------` files, so a bare remove_all cannot unlink
// inside it, and under Shipyard validation each such leftover kept its whole
// per-run TMPDIR on the boot volume.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pulp::test {

inline long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

/// Remove `root` even when a test locked parts of it read-only. Owner rwx is
/// restored on every directory (symlinks are not followed) before the
/// recursive removal. Non-throwing; returns whether `root` is gone.
inline bool remove_tmp_tree(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto root_status = fs::symlink_status(root, ec);
    if (ec || !fs::exists(root_status)) return true;
    if (fs::is_directory(root_status)) {
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
            std::error_code entry_ec;
            // Opened on the next increment, so it must be enterable first.
            const auto entry_status = it->symlink_status(entry_ec);
            if (fs::is_directory(entry_status))
                fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add,
                                entry_ec);
#ifdef _WIN32
            // Windows maps a read-only file to an attribute that blocks deletion.
            else if (fs::is_regular_file(entry_status))
                fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add,
                                entry_ec);
#endif
        }
    }
    ec.clear();
    fs::remove_all(root, ec);
    return !fs::exists(fs::symlink_status(root, ec));
}

/// Paths handed out by unique_tmp_dir/unique_tmp_file, removed at process exit.
struct TmpPathRegistry {
    std::mutex mutex;
    std::vector<std::filesystem::path> paths;
    void add(const std::filesystem::path& p) {
        std::lock_guard<std::mutex> lock(mutex);
        paths.push_back(p);
    }
    ~TmpPathRegistry() {
        for (const auto& p : paths) remove_tmp_tree(p);
    }
};

inline TmpPathRegistry& tmp_path_registry() {
    static TmpPathRegistry registry;
    return registry;
}

/// A fresh, empty, process-unique temp directory named `<prefix><pid>-<n>`.
/// Removed at process exit.
inline std::filesystem::path unique_tmp_dir(const std::string& prefix) {
    static std::atomic<int> counter{0};
    auto p = std::filesystem::temp_directory_path() /
             (prefix + std::to_string(current_pid()) + "-" +
              std::to_string(counter.fetch_add(1) + 1));
    remove_tmp_tree(p);
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    tmp_path_registry().add(p);
    return p;
}

/// A process-unique temp FILE path `<prefix><pid>-<n><ext>` (not created).
/// Whatever the test creates there is removed at process exit.
inline std::filesystem::path unique_tmp_file(const std::string& prefix,
                                             const std::string& ext = "") {
    static std::atomic<int> counter{0};
    auto p = std::filesystem::temp_directory_path() /
             (prefix + std::to_string(current_pid()) + "-" +
              std::to_string(counter.fetch_add(1) + 1) + ext);
    tmp_path_registry().add(p);
    return p;
}

}  // namespace pulp::test
