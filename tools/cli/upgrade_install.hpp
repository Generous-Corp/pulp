// upgrade_install.hpp -- helpers for installing release-archive payloads.
//
// The pre-Phase-8 C++ CLI can upgrade directly into a post-cutover
// Rust release archive. In that archive `pulp` is the Rust binary and
// `pulp-cpp` is the C++ delegate required by fallthrough commands, so
// the old one-file self-replace path must also copy sibling artifacts.

#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::cli::upgrade_install {

namespace fs = std::filesystem;

enum class WindowsMcpEnvAction {
    configure_fresh,
    update_managed,
    preserve_user_override,
};

inline WindowsMcpEnvAction decide_windows_mcp_env_action(
    std::wstring_view current_value, std::wstring_view managed_value) {
    if (current_value.empty()) {
        return WindowsMcpEnvAction::configure_fresh;
    }
    if (!managed_value.empty() && current_value == managed_value) {
        return WindowsMcpEnvAction::update_managed;
    }
    return WindowsMcpEnvAction::preserve_user_override;
}

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
        malformed_profile,  // managed marker block is missing, duplicated, or reversed
        empty_dir,          // install dir was empty
    };
    Status status = Status::already_on_path;
    fs::path profile;     // the profile considered/edited
    std::string line;     // the export line written (or that would be written)
};

inline std::string quote_sh_profile_value(const std::string& value) {
    std::string quoted{"'"};
    for (const char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += '\'';
    return quoted;
}

inline std::string quote_fish_profile_value(const std::string& value) {
    std::string quoted{"'"};
    for (const char c : value) {
        if (c == '\\' || c == '\'') quoted += '\\';
        quoted += c;
    }
    quoted += '\'';
    return quoted;
}

inline bool has_active_mcp_assignment(const std::string& content) {
    std::istringstream lines(content);
    std::string line;
    constexpr std::string_view variable = "PULP_MCP_BINARY";
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        auto statement = line.substr(first);
        if (statement.rfind("export", 0) == 0 &&
            statement.size() > 6 &&
            (statement[6] == ' ' || statement[6] == '\t')) {
            const auto after_export = statement.find_first_not_of(" \t", 6);
            statement = after_export == std::string::npos
                ? std::string{}
                : statement.substr(after_export);
        }
        if (statement.rfind(variable, 0) == 0) {
            const auto equals = statement.find_first_not_of(
                " \t", variable.size());
            if (equals != std::string::npos && statement[equals] == '=') {
                return true;
            }
        }
        if (statement.rfind("set", 0) == 0 &&
            statement.size() > 3 &&
            (statement[3] == ' ' || statement[3] == '\t')) {
            std::istringstream tokens(statement);
            std::string token;
            tokens >> token;  // set
            bool erases_or_queries = false;
            while (tokens >> token) {
                if (token == "-e" || token == "--erase" ||
                    token == "-q" || token == "--query") {
                    erases_or_queries = true;
                } else if (token.size() > 1 && token[0] == '-' &&
                           token[1] != '-' &&
                           (token.find('e', 1) != std::string::npos ||
                            token.find('q', 1) != std::string::npos)) {
                    erases_or_queries = true;
                } else if (!token.empty() && token[0] != '-') {
                    if (token == variable && !erases_or_queries) return true;
                    break;
                }
            }
        }
    }
    return false;
}

inline bool has_active_path_assignment(const std::string& content,
                                       const std::string& dir) {
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        auto statement = line.substr(first);
        if (statement.rfind("export", 0) == 0 &&
            statement.size() > 6 &&
            (statement[6] == ' ' || statement[6] == '\t')) {
            const auto after_export = statement.find_first_not_of(" \t", 6);
            statement = after_export == std::string::npos
                ? std::string{}
                : statement.substr(after_export);
        }
        if (statement.rfind("PATH", 0) == 0) {
            const auto equals = statement.find_first_not_of(" \t", 4);
            if (equals != std::string::npos && statement[equals] == '=' &&
                statement.find(dir, equals + 1) != std::string::npos) {
                return true;
            }
        }
        if (statement.rfind("set", 0) == 0 &&
            statement.size() > 3 &&
            (statement[3] == ' ' || statement[3] == '\t')) {
            std::istringstream tokens(statement);
            std::string token;
            tokens >> token;  // set
            while (tokens >> token) {
                if (!token.empty() && token[0] == '-') continue;
                if (token == "PATH" &&
                    statement.find(dir) != std::string::npos) {
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

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
        out.line = "set -gx PATH " + quote_fish_profile_value(dir) + " $PATH";
    } else if (shell_name == "bash") {
        out.profile = fs::exists(home / ".bash_profile") ? home / ".bash_profile"
                                                          : home / ".bashrc";
        out.line = "export PATH=" + quote_sh_profile_value(dir) + ":$PATH";
    } else if (shell_name == "zsh") {
        out.profile = home / ".zshrc";
        out.line = "export PATH=" + quote_sh_profile_value(dir) + ":$PATH";
    } else {
        out.profile = home / ".profile";
        out.line = "export PATH=" + quote_sh_profile_value(dir) + ":$PATH";
    }

    // Idempotent: don't double-add if the profile already mentions the dir.
    {
        std::ifstream in(out.profile);
        if (in) {
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            if (has_active_path_assignment(content, dir)) {
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

inline PathEnsureOutcome ensure_mcp_binary_env_in_profile(
    const fs::path& profile, const std::string& line) {
    PathEnsureOutcome out;
    out.profile = profile;
    out.line = line;
    constexpr std::string_view managed_start =
        "# >>> Pulp MCP (managed by installer) >>>";
    constexpr std::string_view managed_end =
        "# <<< Pulp MCP (managed by installer) <<<";
    {
        std::ifstream in(out.profile);
        if (in) {
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            std::istringstream marker_lines(content);
            std::string marker_line;
            std::size_t line_number = 0;
            std::size_t start_count = 0;
            std::size_t end_count = 0;
            std::size_t start_line = 0;
            std::size_t end_line = 0;
            while (std::getline(marker_lines, marker_line)) {
                ++line_number;
                if (marker_line == managed_start) {
                    ++start_count;
                    start_line = line_number;
                } else if (marker_line == managed_end) {
                    ++end_count;
                    end_line = line_number;
                }
            }
            if (start_count != 0 || end_count != 0) {
                if (start_count != 1 || end_count != 1 ||
                    start_line >= end_line) {
                    // Never truncate a user profile around a partial/corrupt
                    // managed block. Preserve it for explicit manual repair.
                    out.status = PathEnsureOutcome::Status::malformed_profile;
                    return out;
                }
                std::istringstream lines(content);
                std::ostringstream updated;
                std::string line;
                bool in_managed = false;
                while (std::getline(lines, line)) {
                    if (line == managed_start) {
                        updated << managed_start << "\n"
                                << out.line << "\n";
                        in_managed = true;
                        continue;
                    }
                    if (in_managed && line == managed_end) {
                        updated << managed_end << "\n";
                        in_managed = false;
                        continue;
                    }
                    if (!in_managed) updated << line << "\n";
                }
                // Dotfile managers commonly make ~/.zshrc et al. symlinks.
                // Replace the resolved target atomically so an upgrade never
                // destroys the user's link itself.
                auto replacement_target = out.profile;
                std::error_code symlink_ec;
                if (fs::is_symlink(fs::symlink_status(out.profile, symlink_ec)) &&
                    !symlink_ec) {
                    auto resolved = fs::canonical(out.profile, symlink_ec);
                    if (symlink_ec) {
                        out.status = PathEnsureOutcome::Status::profile_unwritable;
                        return out;
                    }
                    replacement_target = std::move(resolved);
                }
                auto replacement = replacement_target;
                replacement += ".pulp-mcp.tmp";
                std::ofstream rewrite(replacement, std::ios::trunc);
                if (!rewrite) {
                    out.status = PathEnsureOutcome::Status::profile_unwritable;
                    return out;
                }
                rewrite << updated.str();
                rewrite.close();
                if (!rewrite) {
                    std::error_code cleanup_ec;
                    fs::remove(replacement, cleanup_ec);
                    out.status = PathEnsureOutcome::Status::profile_unwritable;
                    return out;
                }
                std::error_code permission_ec;
                const auto original_permissions =
                    fs::status(replacement_target, permission_ec).permissions();
                if (!permission_ec) {
                    fs::permissions(replacement, original_permissions,
                                    fs::perm_options::replace, permission_ec);
                }
                std::error_code replace_ec;
                fs::rename(replacement, replacement_target, replace_ec);
                if (replace_ec) {
                    std::error_code cleanup_ec;
                    fs::remove(replacement, cleanup_ec);
                    out.status = PathEnsureOutcome::Status::profile_unwritable;
                    return out;
                }
                out.status = PathEnsureOutcome::Status::added;
                return out;
            }
            if (has_active_mcp_assignment(content)) {
                out.status = PathEnsureOutcome::Status::already_in_profile;
                return out;
            }
        }
    }

    std::error_code ec;
    fs::create_directories(out.profile.parent_path(), ec);
    std::ofstream app(out.profile, std::ios::app);
    if (!app) { out.status = PathEnsureOutcome::Status::profile_unwritable; return out; }
    app << "\n" << managed_start << "\n"
        << out.line << "\n"
        << managed_end << "\n";
    out.status = PathEnsureOutcome::Status::added;
    return out;
}

inline PathEnsureOutcome ensure_mcp_binary_env(const fs::path& mcp_binary,
                                               const std::string& shell_name,
                                               const fs::path& home,
                                               bool opt_out) {
    PathEnsureOutcome out;
    if (opt_out) { out.status = PathEnsureOutcome::Status::skipped_opt_out; return out; }
    if (mcp_binary.empty()) {
        out.status = PathEnsureOutcome::Status::empty_dir;
        return out;
    }
    if (home.empty()) { out.status = PathEnsureOutcome::Status::no_home; return out; }

    const auto binary_value = shell_name == "fish"
        ? quote_fish_profile_value(mcp_binary.string())
        : quote_sh_profile_value(mcp_binary.string());
    const auto line = shell_name == "fish"
        ? "set -gx PULP_MCP_BINARY " + binary_value
        : shell_name == "bash"
            ? "if [ -z \"${PULP_MCP_BINARY+x}\" ]; then export "
              "PULP_MCP_BINARY=" + binary_value + "; fi"
            : "export PULP_MCP_BINARY=" + binary_value;

    std::vector<fs::path> profiles;
    if (shell_name == "fish") {
        profiles.push_back(home / ".config" / "fish" / "config.fish");
    } else if (shell_name == "bash") {
        // Interactive non-login Bash reads .bashrc; login Bash reads
        // .bash_profile when present and otherwise falls back to .profile.
        // Manage both without adding source-chain hooks that could
        // unexpectedly execute the user's whole .bashrc.
        profiles.push_back(home / ".bashrc");
        if (fs::exists(home / ".bash_profile")) {
            profiles.push_back(home / ".bash_profile");
        } else {
            profiles.push_back(home / ".profile");
        }
    } else if (shell_name == "zsh") {
        profiles.push_back(home / ".zshrc");
    } else {
        profiles.push_back(home / ".profile");
    }

    PathEnsureOutcome first_outcome;
    bool have_outcome = false;
    bool added = false;
    for (const auto& profile : profiles) {
        auto profile_outcome =
            ensure_mcp_binary_env_in_profile(profile, line);
        if (!have_outcome ||
            (profile_outcome.status == PathEnsureOutcome::Status::added &&
             !added)) {
            first_outcome = profile_outcome;
            have_outcome = true;
        }
        if (profile_outcome.status ==
            PathEnsureOutcome::Status::profile_unwritable) {
            return profile_outcome;
        }
        if (profile_outcome.status ==
            PathEnsureOutcome::Status::malformed_profile) {
            return profile_outcome;
        }
        added = added ||
            profile_outcome.status == PathEnsureOutcome::Status::added;
    }
    return first_outcome;
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

inline std::string mcp_binary_name() {
#ifdef _WIN32
    return "pulp-mcp.exe";
#else
    return "pulp-mcp";
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

inline fs::path installed_mcp_server(const std::vector<fs::path>& installed) {
    const auto mcp_name = mcp_binary_name();
    for (const auto& path : installed) {
        if (path.filename() == mcp_name) return path;
    }
    return {};
}

}  // namespace pulp::cli::upgrade_install
