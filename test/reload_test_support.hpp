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

#include <atomic>
#include <filesystem>
#include <string>

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

/// A fresh, empty, process-unique temp directory named `<prefix><pid>-<n>`.
inline std::filesystem::path unique_tmp_dir(const std::string& prefix) {
    static std::atomic<int> counter{0};
    auto p = std::filesystem::temp_directory_path() /
             (prefix + std::to_string(current_pid()) + "-" +
              std::to_string(counter.fetch_add(1) + 1));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

/// A process-unique temp FILE path `<prefix><pid>-<n><ext>` (not created).
inline std::filesystem::path unique_tmp_file(const std::string& prefix,
                                             const std::string& ext = "") {
    static std::atomic<int> counter{0};
    return std::filesystem::temp_directory_path() /
           (prefix + std::to_string(current_pid()) + "-" +
            std::to_string(counter.fetch_add(1) + 1) + ext);
}

}  // namespace pulp::test
