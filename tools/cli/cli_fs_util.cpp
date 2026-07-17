// SPDX-License-Identifier: MIT
//
// cli_fs_util.cpp — see cli_fs_util.hpp.

#include "cli_fs_util.hpp"

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
    if (rel.empty() || rel.is_absolute()) return false;
    for (const auto& part : rel) {
        const auto s = part.string();
        if (s.empty() || s == "." || s == "..") return false;
    }
    return true;
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
