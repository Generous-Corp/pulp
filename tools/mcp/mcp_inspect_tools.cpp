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
using Kind = InspectorMcpToolDescriptor::Kind;

constexpr auto kInspectorMcpTools = std::to_array<InspectorMcpToolDescriptor>({
    {"pulp_inspect_list", "", "Discover owner-private live inspector publications without requiring a source checkout.", Kind::list, inspector_field_none, inspector_field_none, false},
    {"pulp_inspect_capabilities", methods::kSessionGetCapabilities, "Authenticate to a selected session and return its effective capability policy.", Kind::capabilities},
    {"pulp_inspect_context", methods::kInspectorGetAgentContext, "Read build, plugin, window, processing, hot-reload, tweak, and issue context for a selected session.", Kind::context},
    {"pulp_inspect_dom", methods::kDOMGetDocument, "Read the live view tree from an explicitly inspector-enabled standalone session.", Kind::raw},
    {"pulp_inspect_params", methods::kStateGetParameters, "Read the live parameter catalog from an explicitly inspector-enabled standalone session.", Kind::parameters},
    {"pulp_inspect_value_channels", methods::kStateGetValueChannels, "Read the value-channel catalog advertised by an explicitly inspector-enabled session.", Kind::raw},
    {"pulp_inspect_set_param", methods::kStateSetParameter, "Set one live parameter through a typed request and same-connection controller lease.", Kind::set_parameter, inspector_field_id | inspector_field_value | inspector_field_normalized, inspector_field_id | inspector_field_value},
    {"pulp_inspect_screenshot", methods::kCaptureScreenshot, "Capture the selected standalone instance through its compositor-backed inspector surface.", Kind::screenshot},
    {"pulp_inspect_evaluate", methods::kRuntimeEvaluate, "Request high-risk Runtime.evaluate on an explicitly wired and enabled session.", Kind::evaluate, inspector_field_expression, inspector_field_expression},
    {"pulp_inspect_performance", methods::kPerfGetMetrics, "Read live performance and xrun state from an explicitly inspector-enabled standalone session.", Kind::raw},
    {"pulp_inspect_audio", methods::kAudioGetConfig, "Read the selected standalone instance's live audio configuration.", Kind::raw},
    {"pulp_motion_start_trace", methods::kMotionStartTrace, "Start motion tracing on one exact inspector publication and return its trace id.", Kind::motion_start, inspector_field_view_name | inspector_field_metrics | inspector_field_fps, inspector_field_view_name | inspector_field_metrics},
    {"pulp_motion_stop_trace", methods::kMotionStopTrace, "Stop a motion trace on the exact authenticated publication selected by start.", Kind::motion_stop, inspector_field_trace_id, inspector_field_trace_id},
    {"pulp_motion_snapshot", methods::kMotionSnapshot, "Read a motion snapshot from an explicitly inspector-enabled session.", Kind::raw},
    {"pulp_motion_list_traces", methods::kMotionListTraces, "List motion traces on an explicitly inspector-enabled session.", Kind::raw},
    {"pulp_motion_scrub_to", methods::kMotionScrubTo, "Scrub motion state on one exact authenticated publication.", Kind::motion_scrub, inspector_field_frame, inspector_field_frame},
    {"pulp_motion_play", methods::kMotionPlay, "Play motion state on one exact authenticated publication.", Kind::raw},
    {"pulp_motion_pause", methods::kMotionPause, "Pause motion state on one exact authenticated publication.", Kind::raw},
    {"pulp_motion_enable_cost", methods::kMotionEnableCost, "Enable motion cost measurement on one exact authenticated publication.", Kind::raw},
    {"pulp_motion_disable_cost", methods::kMotionDisableCost, "Disable motion cost measurement on one exact authenticated publication.", Kind::raw},
    {"pulp_trace_start", methods::kTraceStartSession, "Start live tracing on one exact inspector publication.", Kind::trace_start, inspector_field_categories | inspector_field_ring_mb},
    {"pulp_trace_stop", methods::kTraceStopSession, "Stop live tracing on the exact authenticated publication selected by start.", Kind::raw},
    {"pulp_trace_snapshot", methods::kTraceSnapshot, "Read live trace status from an explicitly inspector-enabled session.", Kind::raw},
    {"pulp_trace_query", methods::kTraceQuery, "Reserved live Trace.query compatibility surface.", Kind::trace_query, inspector_field_sql | inspector_field_preset | inspector_field_format},
    {"pulp_trace_explain", methods::kTraceExplain, "Reserved live Trace.explain compatibility surface.", Kind::trace_explain, inspector_field_question, inspector_field_question},
});

consteval bool inspector_mcp_tool_names_are_unique() {
    for (std::size_t i = 0; i < kInspectorMcpTools.size(); ++i) {
        if (kInspectorMcpTools[i].name.empty() ||
            kInspectorMcpTools[i].description.empty() ||
            (kInspectorMcpTools[i].kind != Kind::list &&
             kInspectorMcpTools[i].method.empty()))
            return false;
        for (std::size_t j = i + 1; j < kInspectorMcpTools.size(); ++j) {
            if (kInspectorMcpTools[i].name == kInspectorMcpTools[j].name)
                return false;
        }
    }
    return true;
}

static_assert(inspector_mcp_tool_names_are_unique());

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
        if (tool.method.empty())
            continue;
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
    }

    constexpr std::string_view name_marker = "\"name\":\"";
    std::size_t cursor = 0;
    while ((cursor = tools_json.find(name_marker, cursor)) != std::string::npos) {
        const auto name_start = cursor + name_marker.size();
        const auto name_end = tools_json.find('"', name_start);
        if (name_end == std::string::npos)
            return false;
        const std::string_view name(tools_json.data() + name_start, name_end - name_start);
        const bool inspector_shaped = name.starts_with("pulp_inspect_") ||
                                      name.starts_with("pulp_motion_") ||
                                      name.starts_with("pulp_trace_");
        if (inspector_shaped && name != "pulp_inspect_pending_requests" &&
            find_inspector_mcp_tool(name) == nullptr) {
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
