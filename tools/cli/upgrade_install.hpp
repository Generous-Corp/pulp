// upgrade_install.hpp -- helpers for installing release-archive payloads.
//
// The pre-Phase-8 C++ CLI can upgrade directly into a post-cutover
// Rust release archive. In that archive `pulp` is the Rust binary and
// `pulp-cpp` is the C++ delegate required by fallthrough commands, so
// the old one-file self-replace path must also copy sibling artifacts.

#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace pulp::cli::upgrade_install {

namespace fs = std::filesystem;

inline std::string primary_binary_name() {
#ifdef _WIN32
    return "pulp.exe";
#else
    return "pulp";
#endif
}

inline std::string cpp_binary_name() {
#ifdef _WIN32
    return "pulp-cpp.exe";
#else
    return "pulp-cpp";
#endif
}

inline bool has_any_exec_bit(const fs::path& path) {
#ifdef _WIN32
    (void)path;
    return false;
#else
    std::error_code ec;
    const auto perms = fs::status(path, ec).permissions();
    if (ec) return false;
    return (perms & (fs::perms::owner_exec | fs::perms::group_exec |
                     fs::perms::others_exec)) != fs::perms::none;
#endif
}

inline bool should_add_exec_permissions(const fs::path& source) {
    const auto name = source.filename().string();
    return name == primary_binary_name() || name == cpp_binary_name() ||
           has_any_exec_bit(source);
}

inline void add_exec_permissions(const fs::path& path) {
#ifndef _WIN32
    fs::permissions(path,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add);
#else
    (void)path;
#endif
}

inline bool same_path(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    return a.lexically_normal() == b.lexically_normal();
}

inline std::vector<fs::path> install_sibling_payloads(
    const fs::path& extracted_root,
    const fs::path& install_dir,
    const fs::path& primary_binary,
    const fs::path& downloaded_archive) {
    std::vector<fs::path> installed;
    for (const auto& entry : fs::directory_iterator(extracted_root)) {
        if (!entry.is_regular_file()) continue;

        const auto src = entry.path();
        if (same_path(src, primary_binary) || same_path(src, downloaded_archive)) {
            continue;
        }

        const auto dst = install_dir / src.filename();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        if (should_add_exec_permissions(src)) {
            add_exec_permissions(dst);
        }
        installed.push_back(dst);
    }
    return installed;
}

inline bool installed_cpp_delegate(const std::vector<fs::path>& installed) {
    const auto cpp_name = cpp_binary_name();
    for (const auto& path : installed) {
        if (path.filename() == cpp_name) return true;
    }
    return false;
}

}  // namespace pulp::cli::upgrade_install
