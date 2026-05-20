// mcp_tools.hpp — MCP tool-call handlers for pulp-mcp.
//
// Extracted from tools/mcp/pulp_mcp.cpp in the 2026-05 Phase 6 (B4)
// refactor — third cut of the MCP typed-registry split. Each handler
// takes the raw `params` JSON of a tools/call request and returns the
// tool result payload (json_tool_payload-wrapped structured content).
//
// pulp_mcp.cpp's protocol dispatcher routes tool names to these.

#pragma once

#include <string>

namespace pulp_mcp {

std::string handle_build(const std::string& params_json);
std::string handle_test(const std::string& params_json);
std::string handle_status(const std::string& params_json);
std::string handle_validate(const std::string& params_json);
std::string handle_audio_model_status(const std::string& params_json);
std::string handle_audio_model_list(const std::string& params_json);
std::string handle_audio_model_activate(const std::string& params_json);
std::string handle_audio_read_bundle(const std::string& params_json);
std::string handle_audio_excerpt_find(const std::string& params_json);

}  // namespace pulp_mcp
