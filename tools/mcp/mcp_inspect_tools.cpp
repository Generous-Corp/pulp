// mcp_inspect_tools.cpp — Design inspection request MCP handlers.

#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"

#include <pulp/inspect/agent_request_queue.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>

#include <array>
#include <cstddef>
#include <string>

namespace pulp_mcp {
namespace {

using namespace pulp::inspect;

constexpr auto kInspectorMcpTools = std::to_array<InspectorMcpToolDescriptor>({
    {"pulp_trace_start", methods::kTraceStartSession},
    {"pulp_trace_stop", methods::kTraceStopSession},
    {"pulp_trace_snapshot", methods::kTraceSnapshot},
    {"pulp_trace_query", methods::kTraceQuery},
    {"pulp_trace_explain", methods::kTraceExplain},
});

consteval bool inspector_mcp_tool_names_are_unique() {
    for (std::size_t i = 0; i < kInspectorMcpTools.size(); ++i) {
        if (kInspectorMcpTools[i].name.empty() || kInspectorMcpTools[i].method.empty())
            return false;
        for (std::size_t j = i + 1; j < kInspectorMcpTools.size(); ++j) {
            if (kInspectorMcpTools[i].name == kInspectorMcpTools[j].name)
                return false;
        }
    }
    return true;
}

static_assert(inspector_mcp_tool_names_are_unique());

constexpr bool is_inspector_metadata_tool(std::string_view name) {
    return name == "pulp_inspect_profiles" || name == "pulp_inspect_list" ||
           name == "pulp_inspect_capabilities" || name == "pulp_inspect_doctor";
}

} // namespace

std::span<const InspectorMcpToolDescriptor> inspector_mcp_tool_registry() {
    return kInspectorMcpTools;
}

const InspectorMcpToolDescriptor* find_inspector_mcp_tool(std::string_view name) {
    for (const auto& tool : kInspectorMcpTools) {
        if (tool.name == name)
            return &tool;
    }
    return nullptr;
}

std::string_view inspector_mcp_tool_capability(const InspectorMcpToolDescriptor& tool) {
    const auto* method = find_inspector_method(tool.method);
    return method ? capability_id(method->capability) : std::string_view{};
}

bool decorate_inspector_mcp_tool_descriptions(std::string& tools_json) {
    for (const auto& tool : kInspectorMcpTools) {
        const auto capability = inspector_mcp_tool_capability(tool);
        if (capability.empty())
            return false;

        const auto tool_marker = "\"name\":\"" + std::string(tool.name) + "\",\"description\":\"";
        const auto description = tools_json.find(tool_marker);
        if (description == std::string::npos)
            return false;
        const auto insertion = description + tool_marker.size();
        tools_json.insert(insertion, "Inspector method " + std::string(tool.method) +
                                         " (capability " + std::string(capability) + "). ");

        const bool discovers_and_pins_publication = tool.name == "pulp_trace_start";
        if (!discovers_and_pins_publication) {
            constexpr std::string_view input_marker = "\"inputSchema\":{\"type\":\"object\",";
            constexpr std::string_view required_selectors =
                "\"required\":[\"session_id\",\"instance_id\",\"publication_id\"],";
            const auto tool_end = tools_json.find("},{\"name\":", insertion);
            const auto schema = tools_json.find(input_marker, insertion);
            if (schema == std::string::npos ||
                (tool_end != std::string::npos && schema >= tool_end))
                return false;
            const auto properties = tools_json.find("\"properties\":", schema);
            if (properties == std::string::npos ||
                (tool_end != std::string::npos && properties >= tool_end))
                return false;
            const auto existing_required = tools_json.find("\"required\":", schema);
            if (existing_required == std::string::npos || existing_required > properties) {
                tools_json.insert(schema + input_marker.size(), required_selectors);
            } else {
                const auto required_end = tools_json.find(']', existing_required);
                if (required_end == std::string::npos || required_end > properties)
                    return false;
                const std::string_view required_fields(tools_json.data() + existing_required,
                                                       required_end - existing_required);
                if (required_fields.find("\"session_id\"") == std::string_view::npos) {
                    tools_json.insert(required_end,
                                      ",\"session_id\",\"instance_id\",\"publication_id\"");
                }
            }

            constexpr std::string_view optional_exact = "Optional exact";
            constexpr std::string_view required_exact = "Exact";
            auto updated_tool_end = tools_json.find("},{\"name\":", insertion);
            if (updated_tool_end == std::string::npos)
                updated_tool_end = tools_json.size();
            auto optional = tools_json.find(optional_exact, insertion);
            while (optional != std::string::npos && optional < updated_tool_end) {
                tools_json.replace(optional, optional_exact.size(), required_exact);
                updated_tool_end -= optional_exact.size() - required_exact.size();
                optional = tools_json.find(optional_exact, optional + required_exact.size());
            }
        }
    }

    constexpr std::string_view name_marker = "\"name\":\"";
    std::size_t cursor = 0;
    while ((cursor = tools_json.find(name_marker, cursor)) != std::string::npos) {
        const auto name_start = cursor + name_marker.size();
        const auto name_end = tools_json.find('"', name_start);
        if (name_end == std::string::npos)
            return false;
        const std::string_view name(tools_json.data() + name_start, name_end - name_start);
        const bool inspector_shaped = name.starts_with("pulp_trace_");
        if (inspector_shaped && name != "pulp_inspect_pending_requests" &&
            !is_inspector_metadata_tool(name) && find_inspector_mcp_tool(name) == nullptr) {
            return false;
        }
        cursor = name_end + 1;
    }
    return true;
}

// Read the pull-based agent-request queue (.pulp-design-requests.json) for a
// design project and return its not-yet-consumed requests as a JSON array.
// This is an in-process read of the pulp::inspect queue core — no CLI shell-out
// and no audio device — so it degrades honestly: an absent or empty queue is an
// empty array, never an error. `project_dir` locates the queue; when omitted we
// fall back to the enclosing Pulp project root, matching the cwd-based
// resolution the neighboring pulp_inspect_* tools use.
std::string handle_inspect_pending_requests(const std::string& params_json) {
    auto project_dir = extract_string(params_json, "project_dir");
    if (project_dir.empty()) {
        auto root = find_project_root();
        if (root.empty()) {
            return "{\"content\":[{\"type\":\"text\",\"text\":\"Error: not in a Pulp project\"}]}";
        }
        project_dir = root.string();
    }

    const auto path = pulp::inspect::queue_path(project_dir);
    const auto pending = pulp::inspect::read_pending_file(path);

    std::string arr = "[";
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& r = pending[i];
        if (i != 0)
            arr += ",";
        arr += "{\"id\":" + json_string(r.id);
        arr += ",\"text\":" + json_string(r.text);
        arr += ",\"design\":" + json_string(r.design);
        arr += ",\"screen\":" + json_string(r.screen);
        arr += ",\"editmode_state\":" + json_string(r.editmode_state);
        arr += ",\"screenshot_path\":" + json_string(r.screenshot_path);
        arr += ",\"created_at\":" + json_string(r.created_at);
        arr += ",\"consumed\":";
        arr += r.consumed ? "true" : "false";
        arr += "}";
    }
    arr += "]";
    return json_tool_payload(arr);
}

} // namespace pulp_mcp
