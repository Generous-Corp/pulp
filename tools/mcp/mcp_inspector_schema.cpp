#include "mcp_inspector_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_tools.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace pulp_mcp {
namespace {

using ToolDefinition = InspectorMcpToolDescriptor;

void append_required(std::string& output, const ToolDefinition& tool) {
    bool first = true;
    auto append = [&](std::string_view field) {
        output += first ? "\"required\":[" : ",";
        first = false;
        output += json_string(std::string(field));
    };
    const std::pair<std::uint32_t, std::string_view> ordered[] = {
        {inspector_field_id, "id"}, {inspector_field_value, "value"},
        {inspector_field_expression, "expression"},
        {inspector_field_view_name, "view_name"}, {inspector_field_metrics, "metrics"},
        {inspector_field_trace_id, "trace_id"}, {inspector_field_frame, "frame"},
        {inspector_field_question, "question"},
        {inspector_field_kind, "kind"},
        {inspector_field_channel, "channel"},
        {inspector_field_note, "note"},
    };
    for (const auto& [flag, name] : ordered) {
        if ((tool.required_fields & flag) != 0)
            append(name);
    }
    if (tool.exact_selector) {
        append("session_id");
        append("instance_id");
        append("publication_id");
    }
    if (!first)
        output += "],";
}

void append_property(std::string& output, bool& first,
                     std::string_view name, std::string_view schema) {
    if (!first)
        output += ',';
    first = false;
    output += json_string(std::string(name));
    output += ':';
    output += schema;
}

std::string tool_json(const ToolDefinition& tool) {
    std::string output = "{\"name\":" + json_string(std::string(tool.name)) +
                         ",\"description\":" +
                         json_string(std::string(tool.description)) +
                         ",\"inputSchema\":{\"type\":\"object\",";
    append_required(output, tool);
    output += "\"properties\":{";
    bool first = true;
    const auto allowed = tool.allowed_fields;
    if ((allowed & inspector_field_id) != 0)
        append_property(output, first, "id", R"({"type":"integer","minimum":0,"maximum":4294967295})");
    if ((allowed & inspector_field_value) != 0)
        append_property(output, first, "value", R"({"type":"number"})");
    if ((allowed & inspector_field_normalized) != 0)
        append_property(output, first, "normalized", R"({"type":"boolean"})");
    if ((allowed & inspector_field_expression) != 0)
        append_property(output, first, "expression", R"({"type":"string"})");
    if ((allowed & inspector_field_view_name) != 0)
        append_property(output, first, "view_name", R"({"type":"string"})");
    if ((allowed & inspector_field_metrics) != 0)
        append_property(output, first, "metrics", R"({"type":"array","items":{"type":"object","required":["kind"],"properties":{"kind":{"type":"string","enum":["geometry","scroll-geometry","scrollGeometry"]},"name":{"type":"string"},"node_id":{"type":"string"},"properties":{"type":"array","items":{"type":"string"}},"space":{"type":"string"},"source":{}}}})");
    if ((allowed & inspector_field_fps) != 0)
        append_property(output, first, "fps", R"({"type":"integer","minimum":1})");
    if ((allowed & inspector_field_trace_id) != 0)
        append_property(output, first, "trace_id", R"({"type":"integer","minimum":0})");
    if ((allowed & inspector_field_frame) != 0)
        append_property(output, first, "frame", R"({"type":"integer","minimum":0})");
    if ((allowed & inspector_field_categories) != 0)
        append_property(output, first, "categories", R"({"type":"array","items":{"type":"string"}})");
    if ((allowed & inspector_field_ring_mb) != 0)
        append_property(output, first, "ring_mb", R"({"type":"integer","minimum":1,"maximum":512})");
    if ((allowed & inspector_field_sql) != 0)
        append_property(output, first, "sql", R"({"type":"string"})");
    if ((allowed & inspector_field_preset) != 0)
        append_property(output, first, "preset", R"({"type":"string"})");
    if ((allowed & inspector_field_format) != 0)
        append_property(output, first, "format", R"({"type":"string","enum":["json","table","csv"]})");
    if ((allowed & inspector_field_question) != 0)
        append_property(output, first, "question", R"({"type":"string"})");
    if ((allowed & inspector_field_kind) != 0)
        append_property(output, first, "kind", R"({"type":"string","enum":["note_on","note_off"]})");
    if ((allowed & inspector_field_channel) != 0)
        append_property(output, first, "channel", R"({"type":"integer","minimum":1,"maximum":16})");
    if ((allowed & inspector_field_note) != 0)
        append_property(output, first, "note", R"({"type":"integer","minimum":0,"maximum":127})");
    if ((allowed & inspector_field_velocity) != 0)
        append_property(output, first, "velocity", R"({"type":"integer","minimum":0,"maximum":127})");
    if ((allowed & inspector_field_playing) != 0)
        append_property(output, first, "playing", R"({"type":"boolean"})");
    if ((allowed & inspector_field_position_samples) != 0)
        append_property(output, first, "position_samples", R"({"type":"integer","minimum":0})");
    if ((allowed & inspector_field_tempo_bpm) != 0)
        append_property(output, first, "tempo_bpm", R"({"type":"number","minimum":20,"maximum":400})");
    append_property(output, first, "session_id", R"({"type":"string"})");
    append_property(output, first, "instance_id", R"({"type":"string"})");
    append_property(output, first, "publication_id", R"({"type":"string"})");
    output += "}";
    if (tool.kind == InspectorMcpToolDescriptor::Kind::inject_midi ||
        tool.kind == InspectorMcpToolDescriptor::Kind::set_transport)
        output += R"(,"additionalProperties":false)";
    output += "}}";
    return output;
}

} // namespace

std::string live_inspector_tools_json() {
    std::string output;
    for (const auto& tool : inspector_mcp_tool_registry()) {
        if (!output.empty()) output += ',';
        output += tool_json(tool);
    }
    return output;
}

} // namespace pulp_mcp
