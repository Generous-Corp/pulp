// upgrade_install.hpp -- helpers for installing release-archive payloads.
//
// The pre-Phase-8 C++ CLI can upgrade directly into a post-cutover
// Rust release archive. In that archive `pulp` is the Rust binary and
// `pulp-cpp` is the C++ delegate required by fallthrough commands, so
// the old one-file self-replace path must also copy sibling artifacts.

#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace pulp::cli::upgrade_install {

namespace fs = std::filesystem;

// ── PATH ensure (pulp #<path-on-update>) ────────────────────────────────────
//
// The curl install.sh adds the install dir to the user's shell profile, but
// `pulp upgrade` and source/SDK-prefix installs historically did not. A user
// who first got `pulp` via `cmake --install --prefix ~/pulp-sdk` (so it lives
// at ~/pulp-sdk/bin/pulp) could `pulp upgrade` successfully yet still hit
// "command not found" in a fresh shell. This makes the update path self-heal
// PATH the same way install.sh does. Pure on its inputs (env passed in) so it
// is unit-testable without mutating the process environment. Honors the same
// PULP_NO_MODIFY_PATH opt-out as install.sh.

struct PathEnsureOutcome {
    enum class Status {
        already_on_path,    // dir already in $PATH — nothing to do
        already_in_profile, // dir absent from $PATH but the profile already exports it
        added,              // appended an export line to the profile
        skipped_opt_out,    // PULP_NO_MODIFY_PATH=1
        no_home,            // $HOME unavailable — cannot locate a profile
        profile_unwritable, // could not open the profile for append
        empty_dir,          // install dir was empty
    };
    Status status = Status::already_on_path;
    fs::path profile;     // the profile considered/edited
    std::string line;     // the export line written (or that would be written)
};

inline PathEnsureOutcome ensure_dir_on_path(const fs::path& install_dir,
                                            const std::string& path_env,
                                            const std::string& shell_name,
                                            const fs::path& home,
                                            bool opt_out) {
    PathEnsureOutcome out;
    if (opt_out) { out.status = PathEnsureOutcome::Status::skipped_opt_out; return out; }

    const std::string dir = install_dir.string();
    if (dir.empty()) { out.status = PathEnsureOutcome::Status::empty_dir; return out; }

    // Exact path-segment match against $PATH.
    const std::string padded = ":" + path_env + ":";
    if (padded.find(":" + dir + ":") != std::string::npos) {
        out.status = PathEnsureOutcome::Status::already_on_path;
        return out;
    }

    if (home.empty()) { out.status = PathEnsureOutcome::Status::no_home; return out; }

    // Pick the shell profile + export syntax the same way install.sh does.
    if (shell_name == "fish") {
        out.profile = home / ".config" / "fish" / "config.fish";
        out.line = "set -gx PATH " + dir + " $PATH";
    } else if (shell_name == "bash") {
        out.profile = fs::exists(home / ".bash_profile") ? home / ".bash_profile"
                                                          : home / ".bashrc";
        out.line = "export PATH=\"" + dir + ":$PATH\"";
    } else if (shell_name == "zsh") {
        out.profile = home / ".zshrc";
        out.line = "export PATH=\"" + dir + ":$PATH\"";
    } else {
        out.profile = home / ".profile";
        out.line = "export PATH=\"" + dir + ":$PATH\"";
    }

    // Idempotent: don't double-add if the profile already mentions the dir.
    {
        std::ifstream in(out.profile);
        if (in) {
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            if (content.find(dir) != std::string::npos) {
                out.status = PathEnsureOutcome::Status::already_in_profile;
                return out;
            }
        }
    }

    std::error_code ec;
    fs::create_directories(out.profile.parent_path(), ec);  // fish config dir may not exist
    std::ofstream app(out.profile, std::ios::app);
    if (!app) { out.status = PathEnsureOutcome::Status::profile_unwritable; return out; }
    app << "\n# Pulp CLI\n" << out.line << "\n";
    out.status = PathEnsureOutcome::Status::added;
    return out;
}

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
