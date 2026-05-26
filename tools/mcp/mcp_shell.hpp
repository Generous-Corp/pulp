// mcp_shell.hpp — shell-execution + project-root helpers for pulp-mcp.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor. Both the tool handlers (mcp_tools.cpp) and the protocol
// dispatcher (pulp_mcp.cpp) shell out to the `pulp` CLI and need to
// locate the enclosing project root, so these two helpers live in a
// shared header rather than being duplicated.

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace pulp_mcp {

#if defined(_WIN32)
#define PULP_MCP_POPEN _popen
#define PULP_MCP_PCLOSE _pclose
#else
#define PULP_MCP_POPEN popen
#define PULP_MCP_PCLOSE pclose
#endif

// Run `cmd` via popen and capture stdout. On a non-zero exit with no
// captured output, returns a "Command failed with status N" string so
// the caller always has something to surface.
inline std::string exec(const std::string& cmd) {
    std::string result;
    FILE* pipe = PULP_MCP_POPEN(cmd.c_str(), "r");
    if (!pipe) return "Error: failed to run command";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        result += buffer;
    int status = PULP_MCP_PCLOSE(pipe);
    if (status != 0 && result.empty())
        result = "Command failed with status " + std::to_string(status);
    return result;
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

}  // namespace pulp_mcp
