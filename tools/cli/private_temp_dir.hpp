// private_temp_dir.hpp — a fresh download/extraction directory only this user
// can enter, created exclusively per run under a user-owned root.
//
// The CLI verifies a release archive's checksum and then hands it to tar. A
// fixed name in the shared system temp dir would let another local user
// pre-create that directory and swap the archive between the two steps, so
// the sequence runs in a directory that did not exist before this process
// made it and that no other user can open.

#pragma once

#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace pulp::cli {

// Create `<root>/<prefix>-<random>` with owner-only permissions, failing
// rather than reusing a directory that already exists. `root` is created
// owner-only when missing. Returns nullopt when no directory could be made;
// the caller owns cleanup of the returned path.
inline std::optional<std::filesystem::path> make_private_temp_dir(const std::filesystem::path& root,
                                                                  const std::string& prefix) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        fs::create_directories(root, ec);
        if (ec)
            return std::nullopt;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, ec);
    }
#ifndef _WIN32
    // mkdtemp creates the directory 0700 and never returns an existing one.
    std::string tmpl = (root / (prefix + "-XXXXXX")).string();
    if (::mkdtemp(tmpl.data()) == nullptr)
        return std::nullopt;
    return fs::path(tmpl);
#else
    // The per-user temp root on Windows is already private; create_directory
    // reports false when the name exists, which keeps the create exclusive.
    std::random_device rd;
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::ostringstream name;
        name << prefix << '-' << std::hex << rd() << rd();
        const fs::path candidate = root / name.str();
        if (fs::create_directory(candidate, ec) && !ec)
            return candidate;
        ec.clear();
    }
    return std::nullopt;
#endif
}

} // namespace pulp::cli
