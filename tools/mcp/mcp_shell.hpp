// mcp_shell.hpp — shell-execution + project-root helpers for pulp-mcp.
//
// Both the tool handlers (mcp_tools.cpp) and the protocol dispatcher
// (pulp_mcp.cpp) shell out to the `pulp` CLI and need to locate the
// enclosing project root, so these helpers live in a shared header
// rather than being duplicated.

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace pulp_mcp {

#if defined(_WIN32)
#define PULP_MCP_POPEN _popen
#define PULP_MCP_PCLOSE _pclose
#else
#define PULP_MCP_POPEN popen
#define PULP_MCP_PCLOSE pclose
#endif

/// A command's stdout AND its exit status.
struct ExecResult {
    int status = 0;      ///< Exit code. 0 is success; -1 means it never ran.
    std::string output;  ///< Captured stdout.
    bool failed() const { return status != 0; }
};

/// Run `cmd`, capturing stdout and preserving the exit status.
///
/// Use this — not `exec()` — whenever the exit code carries meaning. A command
/// can fail and still print a perfectly well-formed report to stdout: that is
/// exactly what a validation gate or a latency proof does when it DISPROVES
/// something. `exec()` throws the status away whenever there is any output, so
/// routing such a command through it reports the failure to the caller as a
/// success. An agent reading that result would conclude the plugin passed.
inline ExecResult exec_with_status(const std::string& cmd) {
    ExecResult result;
    FILE* pipe = PULP_MCP_POPEN(cmd.c_str(), "r");
    if (!pipe) {
        result.status = -1;
        result.output = "Error: failed to run command";
        return result;
    }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        result.output += buffer;

    const int raw = PULP_MCP_PCLOSE(pipe);
#if defined(_WIN32)
    result.status = raw;
#else
    // pclose() hands back a wait status, not an exit code. A command killed by
    // a signal never "exited", so report it as a generic failure rather than
    // decoding a WEXITSTATUS that is not there.
    result.status = WIFEXITED(raw) ? WEXITSTATUS(raw) : (raw == 0 ? 0 : -1);
#endif
    return result;
}

// Run `cmd` via popen and capture stdout. On a non-zero exit with no
// captured output, returns a "Command failed with status N" string so
// the caller always has something to surface.
//
// Discards the exit status when the command produced output. Prefer
// exec_with_status() when a failure must stay a failure.
inline std::string exec(const std::string& cmd) {
    auto result = exec_with_status(cmd);
    if (result.failed() && result.output.empty())
        result.output = "Command failed with status " + std::to_string(result.status);
    return result.output;
}

inline std::string shell_quote(const std::string& value) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

// Walk up from the current directory to the nearest Pulp source tree
// (a directory with both CMakeLists.txt and core/). Empty path if none.
inline std::filesystem::path find_project_root() {
    auto dir = std::filesystem::current_path();
    while (!dir.empty()) {
        if (std::filesystem::exists(dir / "CMakeLists.txt") &&
            std::filesystem::exists(dir / "core"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

inline std::filesystem::path resolve_cli_binary(const std::filesystem::path& root) {
    const auto build_dir = root / "build";
    const auto cli_dir = build_dir / "tools" / "cli";
    std::vector<std::filesystem::path> candidates;

    auto add_pulp_binary_candidates = [&](const std::filesystem::path& dir) {
        candidates.push_back(dir / "pulp");
        candidates.push_back(dir / "pulp.exe");
    };
    auto add_delegate_candidates = [&](const std::filesystem::path& dir) {
        candidates.push_back(dir / "pulp");
        candidates.push_back(dir / "pulp.exe");
        candidates.push_back(dir / "pulp-cpp");
        candidates.push_back(dir / "pulp-cpp.exe");
    };

    add_pulp_binary_candidates(build_dir);
    for (const char* config : {"Release", "RelWithDebInfo", "Debug", "MinSizeRel"})
        add_pulp_binary_candidates(build_dir / config);

    add_delegate_candidates(cli_dir);
    for (const char* config : {"Release", "RelWithDebInfo", "Debug", "MinSizeRel"})
        add_delegate_candidates(cli_dir / config);

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
#if defined(_WIN32)
    return build_dir / "pulp.exe";
#else
    return build_dir / "pulp";
#endif
}

}  // namespace pulp_mcp
