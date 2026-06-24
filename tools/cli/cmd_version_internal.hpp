// cmd_version_internal.hpp — testable internals for `pulp version`.
//
// Exposes the file-write primitive used when rewriting CMakeLists.txt's
// project(VERSION ...) line so it can be exercised directly by
// test/test_cli_version_write.cpp. Deliberately dependency-free (only
// <fstream>/<filesystem>) so the test compiles without the cli_common
// link surface — same isolation contract as version_diag / project_bump.
#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace pulp::cli::version_internal {

namespace fs = std::filesystem;

// Write `content` to `path`, truncating any existing file. Returns false if
// the stream could not be opened OR if the write/flush failed for any reason
// (e.g. disk full, I/O error, destination is a directory). The caller treats
// a false return as a hard failure and must not report success.
//
// Why the explicit close()+good() check: the ofstream destructor flushes and
// closes the buffer, but by then the stream object is gone and its error
// state is unobservable — a failed final flush would otherwise be lost and the
// truncated file silently reported as a successful write.
inline bool write_text_file_checked(const fs::path& path,
                                     const std::string& content) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << content;
    f.close();          // surface flush/close errors in the stream state
    return f.good();
}

}  // namespace pulp::cli::version_internal
