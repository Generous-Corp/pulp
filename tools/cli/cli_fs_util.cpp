// SPDX-License-Identifier: MIT
//
// cli_fs_util.cpp — see cli_fs_util.hpp.

#include "cli_fs_util.hpp"

#include <pulp/runtime/safe_relative_path.hpp>

#include <atomic>
#include <chrono>
#include <string>

namespace pulp::cli::fsutil {

bool path_is_within(const fs::path& path, const fs::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pit = p.begin();
    auto rit = r.begin();
    for (; rit != r.end(); ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) return false;
    }
    return true;
}

bool safe_archive_rel(const fs::path& rel) {
    return pulp::runtime::is_safe_relative_path(rel);
}

bool is_package_archive_path(const fs::path& path) {
    const auto ext = path.extension().string();
    return ext == ".pulpkit" || ext == ".pulpcontent";
}

fs::path temporary_archive_root() {
    static std::atomic<unsigned> seq{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("pulp-kit-archive-" + std::to_string(ticks) + "-" +
            std::to_string(seq.fetch_add(1)));
}

}  // namespace pulp::cli::fsutil
