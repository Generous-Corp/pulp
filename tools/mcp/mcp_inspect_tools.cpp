// mcp_inspect_tools.cpp — Design inspection request MCP handlers.

#include "mcp_inspect_tools_internal.hpp"
#include "mcp_json.hpp"
#include "mcp_shell.hpp"
#include "mcp_tools.hpp"

#include <pulp/inspect/agent_request_queue.hpp>
#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/protocol.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace pulp_mcp {
namespace {

using namespace pulp::inspect;

constexpr auto kInspectorMcpTools = std::to_array<InspectorMcpToolDescriptor>({
    {"pulp_inspect_dom", methods::kDOMGetDocument},
    {"pulp_inspect_params", methods::kStateGetParameters},
    {"pulp_inspect_value_channels", methods::kStateGetValueChannels},
    {"pulp_inspect_set_param", methods::kStateSetParameter},
    {"pulp_inspect_inject_midi", methods::kTestInjectMidi},
    {"pulp_inspect_set_transport", methods::kTestSetTransport},
    {"pulp_inspect_screenshot", methods::kCaptureScreenshot},
    {"pulp_inspect_evaluate", methods::kRuntimeEvaluate},
    {"pulp_inspect_performance", methods::kPerfGetMetrics},
    {"pulp_inspect_audio", methods::kAudioGetConfig},
    {"pulp_motion_start_trace", methods::kMotionStartTrace},
    {"pulp_motion_stop_trace", methods::kMotionStopTrace},
    {"pulp_motion_snapshot", methods::kMotionSnapshot},
    {"pulp_motion_list_traces", methods::kMotionListTraces},
    {"pulp_motion_scrub_to", methods::kMotionScrubTo},
    {"pulp_motion_play", methods::kMotionPlay},
    {"pulp_motion_pause", methods::kMotionPause},
    {"pulp_motion_enable_cost", methods::kMotionEnableCost},
    {"pulp_motion_disable_cost", methods::kMotionDisableCost},
    {"pulp_trace_start", methods::kTraceStartSession},
    {"pulp_trace_stop", methods::kTraceStopSession},
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

        const bool discovers_and_pins_publication = tool.name == "pulp_motion_start_trace";
        const bool uses_canonical_control =
            tool.name == "pulp_trace_start" || tool.name == "pulp_trace_stop";
        if (!discovers_and_pins_publication && !uses_canonical_control) {
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
        const bool inspector_shaped = name.starts_with("pulp_inspect_") ||
                                      name.starts_with("pulp_motion_") ||
                                      name.starts_with("pulp_trace_");
        if (inspector_shaped && name != "pulp_inspect_pending_requests" &&
            !is_inspector_metadata_tool(name) && find_inspector_mcp_tool(name) == nullptr) {
            return false;
        }
        cursor = name_end + 1;
    }
    return true;
}

namespace detail {
namespace {

bool is_inspector_selector_field(std::string_view name) {
    return name == "session_id" || name == "instance_id" || name == "publication_id";
}

} // namespace

std::optional<InspectorMidiArguments>
parse_inspector_midi_arguments(const choc::value::Value& arguments, std::string& error) {
    for (std::uint32_t index = 0; index < arguments.size(); ++index) {
        const auto name = std::string_view(arguments.getObjectMemberAt(index).name);
        if (!is_inspector_selector_field(name) && name != "kind" && name != "channel" &&
            name != "note" && name != "velocity" && name != "duration_ms") {
            error = "Error: unknown test-input field: " + std::string(name);
            return std::nullopt;
        }
    }
    if (!arguments.hasObjectMember("kind") || !arguments["kind"].isString()) {
        error = "Error: kind must be note_on or note_off";
        return std::nullopt;
    }
    InspectorMidiArguments parsed;
    parsed.kind = std::string(arguments["kind"].getString());
    if (parsed.kind != "note_on" && parsed.kind != "note_off") {
        error = "Error: kind must be note_on or note_off";
        return std::nullopt;
    }
    const auto read_bounded = [&](std::string_view name, std::int64_t minimum, std::int64_t maximum,
                                  std::uint8_t& output) {
        const auto key = std::string(name);
        if (!arguments.hasObjectMember(key) || !arguments[key].isInt())
            return false;
        const auto value = arguments[key].getInt64();
        if (value < minimum || value > maximum)
            return false;
        output = static_cast<std::uint8_t>(value);
        return true;
    };
    std::uint8_t public_channel = 0;
    if (!read_bounded("channel", 1, 16, public_channel)) {
        error = "Error: channel is outside the supported range 1 through 16";
        return std::nullopt;
    }
    parsed.channel = static_cast<std::uint8_t>(public_channel - 1);
    if (!read_bounded("note", 0, 127, parsed.note)) {
        error = "Error: note is outside the supported range 0 through 127";
        return std::nullopt;
    }
    if (arguments.hasObjectMember("velocity")) {
        if (!read_bounded("velocity", 0, 127, parsed.velocity)) {
            error = "Error: velocity is outside the supported range 0 through 127";
            return std::nullopt;
        }
    } else if (parsed.kind == "note_on") {
        error = "Error: velocity is required for note_on";
        return std::nullopt;
    }
    if (parsed.kind == "note_on") {
        if (!arguments.hasObjectMember("duration_ms") || !arguments["duration_ms"].isInt()) {
            error = "Error: duration_ms is required for note_on";
            return std::nullopt;
        }
        const auto duration_ms = arguments["duration_ms"].getInt64();
        if (duration_ms < 1 || duration_ms > 2000) {
            error = "Error: duration_ms must be from 1 through 2000";
            return std::nullopt;
        }
        parsed.hold_duration = std::chrono::milliseconds(duration_ms);
    } else if (arguments.hasObjectMember("duration_ms")) {
        error = "Error: duration_ms applies only to note_on";
        return std::nullopt;
    }
    return parsed;
}

std::optional<InspectorTransportArguments>
parse_inspector_transport_arguments(const choc::value::Value& arguments, std::string& error) {
    for (std::uint32_t index = 0; index < arguments.size(); ++index) {
        const auto name = std::string_view(arguments.getObjectMemberAt(index).name);
        if (!is_inspector_selector_field(name) && name != "playing" && name != "position_samples" &&
            name != "tempo_bpm") {
            error = "Error: unknown test-input field: " + std::string(name);
            return std::nullopt;
        }
    }
    InspectorTransportArguments parsed;
    if (arguments.hasObjectMember("playing")) {
        if (!arguments["playing"].isBool()) {
            error = "Error: playing must be a boolean";
            return std::nullopt;
        }
        parsed.playing = arguments["playing"].getBool();
    }
    if (arguments.hasObjectMember("position_samples")) {
        if (!arguments["position_samples"].isInt() ||
            arguments["position_samples"].getInt64() < 0) {
            error = "Error: position_samples must be a nonnegative integer";
            return std::nullopt;
        }
        parsed.position_samples = arguments["position_samples"].getInt64();
    }
    if (arguments.hasObjectMember("tempo_bpm")) {
        const auto value = arguments["tempo_bpm"];
        if (!value.isInt() && !value.isFloat()) {
            error = "Error: tempo_bpm must be a number";
            return std::nullopt;
        }
        const auto tempo =
            value.isInt() ? static_cast<double>(value.getInt64()) : value.getFloat64();
        if (!std::isfinite(tempo) || tempo < 20.0 || tempo > 400.0) {
            error = "Error: tempo_bpm must be finite and from 20 to 400";
            return std::nullopt;
        }
        parsed.tempo_bpm = tempo;
    }
    if (!parsed.playing && !parsed.position_samples && !parsed.tempo_bpm) {
        error = "Error: set transport requires playing, position_samples, or tempo_bpm";
        return std::nullopt;
    }
    return parsed;
}

} // namespace detail

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
