// cmd_version_internal.hpp — testable internals for `pulp version`.
//
// Exposes the file-write primitive used when rewriting CMakeLists.txt's
// project(VERSION ...) line so it can be exercised directly by
// test/test_cli_version_write.cpp. Deliberately dependency-free (only
// std headers) so the test compiles without the cli_common link surface —
// same isolation contract as version_diag / project_bump.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace pulp::cli::version_internal {

namespace fs = std::filesystem;

// Atomically write `content` to `path`: write a sibling temp file, flush +
// close it (the explicit close() surfaces a failed final flush in the stream
// state — the ofstream destructor would close after the object is gone, losing
// the error), then rename it over `path`. Returns false — leaving any existing
// `path` UNCHANGED — if the temp can't be opened, the write/flush fails (disk
// full, I/O error, destination/parent not writable), or the rename fails.
//
// Atomic, not just checked: this mirrors the temp+rename convention the
// core/state writers use (PropertiesFile, PresetManager, content_registry).
// A crash or failure mid-write can never leave a truncated/corrupt
// destination — which matters most for CMakeLists.txt, the file the whole
// build derives from. `std::filesystem::rename` has replace semantics on all
// platforms (POSIX rename / MoveFileExW(REPLACE_EXISTING) on Windows), so it
// overwrites an existing target. A symlink AT `path` is replaced by a regular
// file (rename targets the link itself, not its referent) — the old in-place
// `ofstream` wrote *through* the link instead; this matches the core/state
// temp+rename writers' behavior and is fine for a VCS-tracked build file.
// This is a flush-to-OS check, not an fsync durability fence — a delayed-
// allocation ENOSPC committed after close() can still slip through; sufficient
// for a dev-time version bump.
inline bool write_text_file_checked(const fs::path& path,
                                     const std::string& content) {
    fs::path tmp = path;
    tmp += ".pulp-tmp";
    {
        std::ofstream f(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f) return false;
        f << content;
        f.close();  // surface flush/close errors while the stream is alive
        if (!f.good()) {
            std::error_code rm;
            fs::remove(tmp, rm);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm;
        fs::remove(tmp, rm);  // don't leave the temp behind on a failed rename
        return false;
    }
    return true;
}

}  // namespace pulp::cli::version_internal
