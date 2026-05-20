// mcp_compat.hpp — project SDK-compatibility resolution for the
// pulp-mcp server.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor — second cut of the MCP typed-registry split.
//
// pulp-mcp ships independently of any given Pulp project. Before
// dispatching a tool that touches a project, the server resolves the
// project's pinned SDK version and refuses tools whose min-SDK floor
// the project hasn't reached. This header is the public surface of
// that compat layer; the parsing internals stay file-local in
// mcp_compat.cpp.

#pragma once

#include <string>

namespace pulp_mcp {

// The minimum SDK version a given tool requires, or empty if the tool
// has no floor. Keyed by MCP tool name (e.g. "pulp_build").
std::string min_sdk_for_tool(const std::string& name);

// Resolve the current project's pinned SDK version (from pulp.toml or
// the project CMakeLists), or empty if no project is detected.
std::string resolve_project_sdk_version();

// Semantic-version compare: <0 if a<b, 0 if equal, >0 if a>b.
// Tolerant of malformed input (treats unparseable components as 0).
int compare_semver(const std::string& a, const std::string& b);

// JSON-RPC error payload for a tool blocked by the project's SDK floor.
std::string compat_error_payload(const std::string& tool_name,
                                 const std::string& min_sdk,
                                 const std::string& project_sdk);

// The `pulp_compat` tool handler — reports the resolved project SDK and
// the server's own version as a structured tool payload.
std::string handle_compat();

}  // namespace pulp_mcp
