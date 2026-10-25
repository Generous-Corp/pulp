// SPDX-License-Identifier: MIT
#pragma once

// The (mtime, size) stamp the scan blacklist and scan cache key a bundle by.
// A bundle directory's own mtime and size say nothing about the code inside
// it (rebuilding the binary in place leaves both alone), so a directory is
// stamped through its largest regular file, which is the executable.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace pulp::host::detail {

struct BundleFileStamp {
    int64_t mtime = 0;
    int64_t size = 0;
    bool ok = false;
};

inline std::filesystem::path bundle_stamped_file(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(path, ec))
        return path;
    fs::path best;
    std::uintmax_t best_size = 0;
    for (fs::recursive_directory_iterator
             it(path, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const auto size = it->file_size(ec);
        if (ec)
            continue;
        if (best.empty() || size > best_size) {
            best = it->path();
            best_size = size;
        }
    }
    return best.empty() ? path : best;
}

inline BundleFileStamp bundle_file_stamp(const std::string& path) {
    namespace fs = std::filesystem;
    BundleFileStamp out;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return out;
    const auto file = bundle_stamped_file(path);
    auto mtime = fs::last_write_time(file, ec);
    if (ec)
        return out;
    out.mtime = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
    if (fs::is_regular_file(file, ec)) {
        out.size = static_cast<int64_t>(fs::file_size(file, ec));
        if (ec)
            return out;
    }
    out.ok = true;
    return out;
}

} // namespace pulp::host::detail
