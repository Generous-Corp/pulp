// mcp_tools.hpp — MCP tool-call handlers for pulp-mcp.
//
// Each handler takes the raw `params` JSON of a tools/call request and
// returns the tool result payload (json_tool_payload-wrapped structured
// content).
//
// pulp_mcp.cpp's protocol dispatcher routes tool names to these.

#pragma once

#include <string>

namespace pulp_mcp {

std::string handle_build(const std::string& params_json);
std::string handle_test(const std::string& params_json);
std::string handle_status(const std::string& params_json);
std::string handle_validate(const std::string& params_json);
std::string handle_kit(const std::string& params_json);
std::string handle_kit_search(const std::string& params_json);
std::string handle_kit_validate(const std::string& params_json);
std::string handle_kit_inspect(const std::string& params_json);
std::string handle_kit_plan(const std::string& params_json);
std::string handle_kit_verify(const std::string& params_json);
std::string handle_kit_apply(const std::string& params_json);
std::string handle_kit_remove(const std::string& params_json);
std::string handle_kit_pack(const std::string& params_json);
std::string handle_kit_publish_check(const std::string& params_json);
std::string handle_kit_init(const std::string& params_json);
std::string handle_content(const std::string& params_json);
std::string handle_content_validate(const std::string& params_json);
std::string handle_content_preview(const std::string& params_json);
std::string handle_content_install(const std::string& params_json);
std::string handle_content_update(const std::string& params_json);
std::string handle_content_list(const std::string& params_json);
std::string handle_content_rescan(const std::string& params_json);
std::string handle_content_remove(const std::string& params_json);
std::string handle_content_reveal(const std::string& params_json);
std::string handle_audio_model_status(const std::string& params_json);
std::string handle_audio_model_list(const std::string& params_json);
std::string handle_audio_model_activate(const std::string& params_json);
std::string handle_audio_read_bundle(const std::string& params_json);
std::string handle_audio_excerpt_find(const std::string& params_json);
std::string handle_audio_probe_json(const std::string& params_json);
std::string handle_audio_scope(const std::string& params_json);
std::string handle_audio_render(const std::string& params_json);
std::string handle_audio_compare(const std::string& params_json);

}  // namespace pulp_mcp
