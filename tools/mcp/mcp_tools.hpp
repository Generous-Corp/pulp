// mcp_tools.hpp — MCP tool-call handlers for pulp-mcp.
//
// Each handler takes the raw `params` JSON of a tools/call request and
// returns the tool result payload (json_tool_payload-wrapped structured
// content).
//
// pulp_mcp.cpp's protocol dispatcher routes tool names to these.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pulp_mcp {

struct InspectorMcpToolDescriptor {
    std::string_view name;
    std::string_view method;
    std::string_view description;
    enum class Kind {
        list,
        capabilities,
        context,
        raw,
        parameters,
        set_parameter,
        screenshot,
        evaluate,
        motion_start,
        motion_stop,
        motion_scrub,
        trace_start,
        trace_query,
        trace_explain,
    } kind = Kind::raw;
    std::uint32_t allowed_fields = 0;
    std::uint32_t required_fields = 0;
    bool exact_selector = true;
};

enum InspectorMcpField : std::uint32_t {
    inspector_field_none = 0,
    inspector_field_id = 1u << 0,
    inspector_field_value = 1u << 1,
    inspector_field_normalized = 1u << 2,
    inspector_field_expression = 1u << 3,
    inspector_field_view_name = 1u << 4,
    inspector_field_metrics = 1u << 5,
    inspector_field_fps = 1u << 6,
    inspector_field_trace_id = 1u << 7,
    inspector_field_frame = 1u << 8,
    inspector_field_categories = 1u << 9,
    inspector_field_ring_mb = 1u << 10,
    inspector_field_sql = 1u << 11,
    inspector_field_preset = 1u << 12,
    inspector_field_format = 1u << 13,
    inspector_field_question = 1u << 14,
};

std::span<const InspectorMcpToolDescriptor> inspector_mcp_tool_registry();
const InspectorMcpToolDescriptor* find_inspector_mcp_tool(std::string_view name);
std::string_view inspector_mcp_tool_capability(const InspectorMcpToolDescriptor& tool);
bool decorate_inspector_mcp_tool_descriptions(std::string& tools_json);

std::string handle_build(const std::string& params_json);
std::string handle_test(const std::string& params_json);
std::string handle_status(const std::string& params_json);
std::string handle_validate(const std::string& params_json);
std::string handle_minos(const std::string& params_json);
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
std::string handle_audio_plugin_inspect(const std::string& params_json);
std::string handle_audio_render(const std::string& params_json);
std::string handle_audio_compare(const std::string& params_json);
std::string handle_timeline_project_open(const std::string& params_json);
std::string handle_timeline_command_apply(const std::string& params_json);
std::string handle_timeline_diff(const std::string& params_json);
std::string handle_timeline_undo(const std::string& params_json);
std::string handle_timeline_redo(const std::string& params_json);
std::string handle_timeline_validate(const std::string& params_json);
std::string handle_timeline_explain(const std::string& params_json);
std::string handle_timeline_render(const std::string& params_json);
std::string handle_timeline_export(const std::string& params_json);
std::string handle_timeline_import(const std::string& params_json);
std::optional<std::string> handle_timeline_tool(std::string_view name,
                                                const std::string& params_json);
std::string handle_inspect_pending_requests(const std::string& params_json);

}  // namespace pulp_mcp
