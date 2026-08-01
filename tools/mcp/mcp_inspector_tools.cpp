#include "mcp_inspector_tools.hpp"

#include "mcp_json.hpp"

#include <pulp/inspect/client_session.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace pulp_mcp {
namespace {

enum class ToolKind {
    list,
    capabilities,
    context,
    dom,
    parameters,
    value_channels,
    set_parameter,
    screenshot,
    evaluate,
    performance,
    audio,
    motion_start,
    motion_stop,
    motion_snapshot,
    motion_list,
    motion_scrub,
    motion_play,
    motion_pause,
    motion_enable_cost,
    motion_disable_cost,
    trace_start,
    trace_stop,
    trace_snapshot,
    trace_query,
    trace_explain,
};

enum Field : std::uint32_t {
    field_none = 0,
    field_id = 1u << 0,
    field_value = 1u << 1,
    field_normalized = 1u << 2,
    field_expression = 1u << 3,
    field_view_name = 1u << 4,
    field_metrics = 1u << 5,
    field_fps = 1u << 6,
    field_trace_id = 1u << 7,
    field_frame = 1u << 8,
    field_categories = 1u << 9,
    field_ring_mb = 1u << 10,
    field_sql = 1u << 11,
    field_preset = 1u << 12,
    field_format = 1u << 13,
    field_question = 1u << 14,
};

struct ToolDefinition {
    std::string_view name;
    std::string_view description;
    std::string_view method;
    ToolKind kind;
    std::uint32_t allowed_fields = field_none;
    std::uint32_t required_fields = field_none;
    bool exact_selector = true;
};

constexpr ToolDefinition kTools[] = {
    {"pulp_inspect_list", "Discover owner-private live inspector publications without requiring a source checkout.", "", ToolKind::list, field_none, field_none, false},
    {"pulp_inspect_capabilities", "Authenticate to a selected session and return its effective capability policy.", "Session.getCapabilities", ToolKind::capabilities},
    {"pulp_inspect_context", "Read build, plugin, window, processing, hot-reload, tweak, and issue context for a selected session.", "Inspector.getAgentContext", ToolKind::context},
    {"pulp_inspect_dom", "Read the live view tree from an explicitly inspector-enabled standalone session.", "DOM.getDocument", ToolKind::dom},
    {"pulp_inspect_params", "Read the live parameter catalog from an explicitly inspector-enabled standalone session.", "State.getParameters", ToolKind::parameters},
    {"pulp_inspect_value_channels", "Read the value-channel catalog advertised by an explicitly inspector-enabled session.", "State.getValueChannels", ToolKind::value_channels},
    {"pulp_inspect_set_param", "Set one live parameter through a typed request and same-connection controller lease.", "State.setParameter", ToolKind::set_parameter, field_id | field_value | field_normalized, field_id | field_value},
    {"pulp_inspect_screenshot", "Capture the selected standalone instance through its compositor-backed inspector surface.", "Capture.screenshot", ToolKind::screenshot},
    {"pulp_inspect_evaluate", "Request high-risk Runtime.evaluate on an explicitly wired and enabled session.", "Runtime.evaluate", ToolKind::evaluate, field_expression, field_expression},
    {"pulp_inspect_performance", "Read live performance and xrun state from an explicitly inspector-enabled standalone session.", "Performance.getMetrics", ToolKind::performance},
    {"pulp_inspect_audio", "Read the selected standalone instance's live audio configuration.", "Audio.getConfig", ToolKind::audio},
    {"pulp_motion_start_trace", "Start motion tracing on one exact inspector publication and return its trace id.", "Motion.startTrace", ToolKind::motion_start, field_view_name | field_metrics | field_fps, field_view_name | field_metrics},
    {"pulp_motion_stop_trace", "Stop a motion trace on the exact authenticated publication selected by start.", "Motion.stopTrace", ToolKind::motion_stop, field_trace_id, field_trace_id},
    {"pulp_motion_snapshot", "Read a motion snapshot from an explicitly inspector-enabled session.", "Motion.snapshot", ToolKind::motion_snapshot},
    {"pulp_motion_list_traces", "List motion traces on an explicitly inspector-enabled session.", "Motion.listTraces", ToolKind::motion_list},
    {"pulp_motion_scrub_to", "Scrub motion state on one exact authenticated publication.", "Motion.scrubTo", ToolKind::motion_scrub, field_frame, field_frame},
    {"pulp_motion_play", "Play motion state on one exact authenticated publication.", "Motion.play", ToolKind::motion_play},
    {"pulp_motion_pause", "Pause motion state on one exact authenticated publication.", "Motion.pause", ToolKind::motion_pause},
    {"pulp_motion_enable_cost", "Enable motion cost measurement on one exact authenticated publication.", "Motion.enableCost", ToolKind::motion_enable_cost},
    {"pulp_motion_disable_cost", "Disable motion cost measurement on one exact authenticated publication.", "Motion.disableCost", ToolKind::motion_disable_cost},
    {"pulp_trace_start", "Start live tracing on one exact inspector publication.", "Trace.startSession", ToolKind::trace_start, field_categories | field_ring_mb},
    {"pulp_trace_stop", "Stop live tracing on the exact authenticated publication selected by start.", "Trace.stopSession", ToolKind::trace_stop},
    {"pulp_trace_snapshot", "Read live trace status from an explicitly inspector-enabled session.", "Trace.snapshot", ToolKind::trace_snapshot},
    {"pulp_trace_query", "Reserved live Trace.query compatibility surface.", "Trace.query", ToolKind::trace_query, field_sql | field_preset | field_format},
    {"pulp_trace_explain", "Reserved live Trace.explain compatibility surface.", "Trace.explain", ToolKind::trace_explain, field_question, field_question},
};

const ToolDefinition* find_tool(std::string_view name) {
    const auto found = std::find_if(
        std::begin(kTools), std::end(kTools),
        [name](const auto& tool) { return tool.name == name; });
    return found == std::end(kTools) ? nullptr : found;
}

std::string error_payload(std::string_view message,
                          std::string_view code = "invalid_arguments") {
    auto payload = json_tool_payload(
        std::string("{\"ok\":false,\"error\":{\"code\":") +
        json_string(std::string(code)) + ",\"message\":" +
        json_string(std::string(message)) + ",\"data\":{}}}");
    payload.insert(payload.size() - 1, ",\"isError\":true");
    return payload;
}

void append_required(std::string& output, const ToolDefinition& tool) {
    bool first = true;
    auto append = [&](std::string_view field) {
        output += first ? "\"required\":[" : ",";
        first = false;
        output += json_string(std::string(field));
    };
    const std::pair<Field, std::string_view> ordered[] = {
        {field_id, "id"}, {field_value, "value"},
        {field_expression, "expression"},
        {field_view_name, "view_name"}, {field_metrics, "metrics"},
        {field_trace_id, "trace_id"}, {field_frame, "frame"},
        {field_question, "question"},
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
    if ((allowed & field_id) != 0)
        append_property(output, first, "id", R"({"type":"integer","minimum":0,"maximum":4294967295})");
    if ((allowed & field_value) != 0)
        append_property(output, first, "value", R"({"type":"number"})");
    if ((allowed & field_normalized) != 0)
        append_property(output, first, "normalized", R"({"type":"boolean"})");
    if ((allowed & field_expression) != 0)
        append_property(output, first, "expression", R"({"type":"string"})");
    if ((allowed & field_view_name) != 0)
        append_property(output, first, "view_name", R"({"type":"string"})");
    if ((allowed & field_metrics) != 0)
        append_property(output, first, "metrics", R"({"type":"array","items":{"type":"object","required":["kind"],"properties":{"kind":{"type":"string","enum":["geometry","scroll-geometry","scrollGeometry"]},"name":{"type":"string"},"node_id":{"type":"string"},"properties":{"type":"array","items":{"type":"string"}},"space":{"type":"string"},"source":{}}}})");
    if ((allowed & field_fps) != 0)
        append_property(output, first, "fps", R"({"type":"integer","minimum":1})");
    if ((allowed & field_trace_id) != 0)
        append_property(output, first, "trace_id", R"({"type":"integer","minimum":0})");
    if ((allowed & field_frame) != 0)
        append_property(output, first, "frame", R"({"type":"integer","minimum":0})");
    if ((allowed & field_categories) != 0)
        append_property(output, first, "categories", R"({"type":"array","items":{"type":"string"}})");
    if ((allowed & field_ring_mb) != 0)
        append_property(output, first, "ring_mb", R"({"type":"integer","minimum":1,"maximum":512})");
    if ((allowed & field_sql) != 0)
        append_property(output, first, "sql", R"({"type":"string"})");
    if ((allowed & field_preset) != 0)
        append_property(output, first, "preset", R"({"type":"string"})");
    if ((allowed & field_format) != 0)
        append_property(output, first, "format", R"({"type":"string","enum":["json","table","csv"]})");
    if ((allowed & field_question) != 0)
        append_property(output, first, "question", R"({"type":"string"})");
    append_property(output, first, "session_id", R"({"type":"string"})");
    append_property(output, first, "instance_id", R"({"type":"string"})");
    append_property(output, first, "publication_id", R"({"type":"string"})");
    output += "}}}";
    return output;
}

struct Arguments {
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::optional<std::uint32_t> parameter_id;
    std::optional<double> value;
    bool normalized = false;
    std::optional<std::string> expression;
    std::optional<std::string> view_name;
    std::optional<std::string> metrics_json;
    std::optional<std::int64_t> fps;
    std::optional<std::int64_t> trace_id;
    std::optional<std::int64_t> frame;
    std::optional<std::string> categories_json;
    std::optional<std::int64_t> ring_mb;
    std::optional<std::string> sql;
    std::optional<std::string> preset;
    std::optional<std::string> format;
    std::optional<std::string> question;
};

std::optional<std::string> read_string(
    const choc::value::ValueView& object, std::string_view name,
    bool required, std::optional<std::string>& output) {
    if (!object.hasObjectMember(std::string(name))) {
        if (required)
            return std::string(name) + " is required";
        return std::nullopt;
    }
    const auto value = object[std::string(name).c_str()];
    if (!value.isString())
        return std::string(name) + " must be a string";
    output = std::string(value.getString());
    return std::nullopt;
}

std::optional<std::string> read_integer(
    const choc::value::ValueView& object, std::string_view name,
    bool required, std::optional<std::int64_t>& output,
    std::int64_t minimum = std::numeric_limits<std::int64_t>::min(),
    std::int64_t maximum = std::numeric_limits<std::int64_t>::max()) {
    if (!object.hasObjectMember(std::string(name))) {
        if (required)
            return std::string(name) + " is required";
        return std::nullopt;
    }
    const auto value = object[std::string(name).c_str()];
    if (!value.isInt())
        return std::string(name) + " must be an integer";
    const auto number = value.getInt64();
    if (number < minimum || number > maximum) {
        if (name == "id")
            return "id must be an integer in the uint32 range";
        return std::string(name) + " is outside the supported range";
    }
    output = number;
    return std::nullopt;
}

std::optional<std::string> parse_arguments(
    const ToolDefinition& tool, const choc::value::ValueView& object,
    Arguments& output) {
    if (!object.isObject())
        return "inspector tool arguments must be a JSON object";
    auto selector = [&](std::string_view name, std::string& destination) ->
        std::optional<std::string> {
        if (!object.hasObjectMember(std::string(name))) {
            if (tool.exact_selector)
                return std::string(name) +
                       " is required for an exact inspector selection";
            return std::nullopt;
        }
        const auto value = object[std::string(name).c_str()];
        if (!value.isString())
            return std::string(name) + " must be a string";
        destination = std::string(value.getString());
        if (tool.exact_selector && destination.empty())
            return std::string(name) +
                   " is required for an exact inspector selection";
        return std::nullopt;
    };
    if (auto error = selector("session_id", output.session_id)) return error;
    if (auto error = selector("instance_id", output.instance_id)) return error;
    if (auto error = selector("publication_id", output.publication_id)) return error;

    const auto required = tool.required_fields;
    const auto allowed = tool.allowed_fields;
    if ((allowed & field_id) != 0) {
        std::optional<std::int64_t> id;
        if (auto error = read_integer(object, "id", (required & field_id) != 0,
                                      id, 0, 4294967295LL)) return error;
        if (id) output.parameter_id = static_cast<std::uint32_t>(*id);
    }
    if ((allowed & field_value) != 0) {
        if (!object.hasObjectMember("value")) {
            if ((required & field_value) != 0) return "value is required";
        } else {
            const auto value = object["value"];
            if (!value.isInt() && !value.isFloat())
                return "value must be a number";
            const auto number = value.getFloat64();
            if (!std::isfinite(number))
                return "value must be finite";
            output.value = number;
        }
    }
    if ((allowed & field_normalized) != 0 &&
        object.hasObjectMember("normalized")) {
        const auto value = object["normalized"];
        if (!value.isBool()) return "normalized must be a boolean";
        output.normalized = value.getBool();
    }
    if ((allowed & field_expression) != 0) {
        if (auto error = read_string(
                object, "expression",
                (required & field_expression) != 0, output.expression))
            return error;
        if ((required & field_expression) != 0 && output.expression->empty())
            return "expression must not be empty";
    }
    if ((allowed & field_view_name) != 0)
        if (auto error = read_string(object, "view_name", (required & field_view_name) != 0, output.view_name)) return error;
    if ((allowed & field_metrics) != 0) {
        if (!object.hasObjectMember("metrics")) {
            if ((required & field_metrics) != 0) return "metrics is required";
        } else if (!object["metrics"].isArray()) {
            return "metrics must be an array";
        } else {
            const auto metrics = object["metrics"];
            for (std::uint32_t index = 0; index < metrics.size(); ++index) {
                const auto metric = metrics[index];
                if (!metric.isObject()) return "metrics entries must be objects";
                if (!metric.hasObjectMember("kind") ||
                    !metric["kind"].isString())
                    return "metrics entries require a string kind";
                const auto kind = metric["kind"].getString();
                if (kind != "geometry" && kind != "scroll-geometry" &&
                    kind != "scrollGeometry")
                    return "metrics kind is unsupported";
                for (const char* field : {"name", "node_id", "space"}) {
                    if (metric.hasObjectMember(field) &&
                        !metric[field].isString())
                        return std::string("metrics ") + field +
                               " must be a string";
                }
                if (metric.hasObjectMember("properties")) {
                    const auto properties = metric["properties"];
                    if (!properties.isArray())
                        return "metrics properties must be an array";
                    for (std::uint32_t property = 0;
                         property < properties.size(); ++property)
                        if (!properties[property].isString())
                            return "metrics properties entries must be strings";
                }
            }
            output.metrics_json = choc::json::toString(metrics, false);
        }
    }
    if ((allowed & field_fps) != 0)
        if (auto error = read_integer(object, "fps", false, output.fps, 1)) return error;
    if ((allowed & field_trace_id) != 0)
        if (auto error = read_integer(object, "trace_id", (required & field_trace_id) != 0, output.trace_id, 0)) return error;
    if ((allowed & field_frame) != 0)
        if (auto error = read_integer(object, "frame", (required & field_frame) != 0, output.frame, 0)) return error;
    if ((allowed & field_categories) != 0) {
        if (object.hasObjectMember("categories")) {
            const auto categories = object["categories"];
            if (!categories.isArray()) return "categories must be an array";
            for (std::uint32_t index = 0; index < categories.size(); ++index)
                if (!categories[index].isString())
                    return "categories entries must be strings";
            output.categories_json = choc::json::toString(categories, false);
        }
    }
    if ((allowed & field_ring_mb) != 0)
        if (auto error = read_integer(object, "ring_mb", false, output.ring_mb, 1, 512)) return error;
    if ((allowed & field_sql) != 0)
        if (auto error = read_string(object, "sql", false, output.sql)) return error;
    if ((allowed & field_preset) != 0)
        if (auto error = read_string(object, "preset", false, output.preset)) return error;
    if ((allowed & field_format) != 0) {
        if (auto error = read_string(object, "format", false, output.format)) return error;
        if (output.format && *output.format != "json" &&
            *output.format != "table" && *output.format != "csv")
            return "format must be json, table, or csv";
    }
    if ((allowed & field_question) != 0)
        if (auto error = read_string(object, "question", (required & field_question) != 0, output.question)) return error;
    return std::nullopt;
}

struct CommandResult {
    bool success = false;
    std::string output;
};

CommandResult connection_failure(
    const pulp::inspect::InspectorClientFailure& failure) {
    return {false, std::string("{\"ok\":false,\"error\":{\"code\":") +
                       json_string(failure.code) + ",\"message\":" +
                       json_string(failure.message) + ",\"data\":" +
                       (failure.data_json.empty() ? "{}" : failure.data_json) +
                       "}}"};
}

template <typename Result>
CommandResult typed_response(
    const Result& result,
    const pulp::inspect::InspectorDiscoveryRecord& selected) {
    const auto identity = std::string("\"session\":{\"session_id\":") +
                          json_string(selected.session_id) +
                          ",\"instance_id\":" + json_string(selected.instance_id) +
                          ",\"publication_id\":" + json_string(selected.publication_id) + "}";
    if (!result) {
        return {false, std::string("{\"ok\":false,") + identity +
                           ",\"error\":{\"code\":" +
                           json_string(result.failure.code) +
                           ",\"message\":" + json_string(result.failure.message) +
                           ",\"data\":" +
                           (result.failure.data_json.empty() ? "{}" : result.failure.data_json) +
                           "}}"};
    }
    return {true, std::string("{\"ok\":true,") + identity +
                      ",\"result\":" + result.response_json + "}"};
}

CommandResult raw_response(
    const pulp::inspect::InspectorMessage& response,
    const pulp::inspect::InspectorDiscoveryRecord& selected) {
    const auto identity = std::string("\"session\":{\"session_id\":") +
                          json_string(selected.session_id) +
                          ",\"instance_id\":" + json_string(selected.instance_id) +
                          ",\"publication_id\":" + json_string(selected.publication_id) + "}";
    if (response.is_error) {
        return {false, std::string("{\"ok\":false,") + identity +
                           ",\"error\":{\"code\":" +
                           json_string(response.error_code) +
                           ",\"message\":" + json_string(response.params_json) +
                           ",\"data\":" +
                           (response.error_data_json.empty() ? "{}" : response.error_data_json) +
                           "}}"};
    }
    return {true, std::string("{\"ok\":true,") + identity +
                      ",\"result\":" + response.params_json + "}"};
}

std::string command_payload(CommandResult command) {
    auto payload = json_tool_payload(command.output);
    if (!command.success)
        payload.insert(payload.size() - 1, ",\"isError\":true");
    return payload;
}

std::string list_sessions(const Arguments& arguments) {
    pulp::inspect::InspectorClientFailure failure;
    const auto records = pulp::inspect::discover_inspector_sessions(
        {.session_id = arguments.session_id,
         .instance_id = arguments.instance_id,
         .publication_id = arguments.publication_id},
        &failure);
    if (!failure.code.empty())
        return command_payload(connection_failure(failure));
    std::string output =
        "{\"ok\":true,\"schemaVersion\":\"pulp.inspect.sessions.v1\",\"sessions\":[";
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (index != 0) output += ',';
        const auto& record = records[index];
        output += "{\"sessionId\":" + json_string(record.session_id) +
                  ",\"instanceId\":" + json_string(record.instance_id) +
                  ",\"publicationId\":" + json_string(record.publication_id) +
                  ",\"pluginId\":" + json_string(record.plugin_id) +
                  ",\"profile\":" +
                  json_string(std::string(pulp::inspect::profile_id(record.profile))) + "}";
    }
    output += "]}";
    return command_payload({true, std::move(output)});
}

std::string make_params(const ToolDefinition& tool, const Arguments& args) {
    switch (tool.kind) {
        case ToolKind::set_parameter: return "{}";
        case ToolKind::evaluate:
            return args.expression ? "{\"expression\":" + json_string(*args.expression) + "}" : "{}";
        case ToolKind::motion_start: {
            auto result = "{\"view_name\":" + json_string(*args.view_name) +
                          ",\"metrics\":" + *args.metrics_json;
            if (args.fps) result += ",\"fps\":" + std::to_string(*args.fps);
            return result + "}";
        }
        case ToolKind::motion_stop:
            return "{\"trace_id\":" + std::to_string(*args.trace_id) + "}";
        case ToolKind::motion_scrub:
            return "{\"frame\":" + std::to_string(*args.frame) + "}";
        case ToolKind::trace_start: {
            std::string result = "{";
            if (args.categories_json) result += "\"categories\":" + *args.categories_json;
            if (args.ring_mb) {
                if (result.size() != 1) result += ',';
                result += "\"ring_mb\":" + std::to_string(*args.ring_mb);
            }
            return result + "}";
        }
        case ToolKind::trace_query: {
            std::string result = "{";
            auto append = [&](std::string_view name, const std::optional<std::string>& value) {
                if (!value) return;
                if (result.size() != 1) result += ',';
                result += json_string(std::string(name)) + ':' + json_string(*value);
            };
            append("sql", args.sql);
            append("preset", args.preset);
            append("format", args.format);
            return result + "}";
        }
        case ToolKind::trace_explain:
            return "{\"question\":" + json_string(*args.question) + "}";
        default: return "{}";
    }
}

std::string execute(const ToolDefinition& tool, const Arguments& args) {
    if (tool.kind == ToolKind::list)
        return list_sessions(args);
    pulp::inspect::InspectorClientFailure failure;
    auto client = pulp::inspect::InspectorClientSession::connect(
        {.session_id = args.session_id,
         .instance_id = args.instance_id,
         .publication_id = args.publication_id},
        &failure);
    if (!client)
        return command_payload(connection_failure(failure));
    if (tool.kind == ToolKind::capabilities)
        return command_payload(typed_response(client->read_capabilities(), client->record()));
    if (tool.kind == ToolKind::context)
        return command_payload(typed_response(client->read_agent_context(), client->record()));
    if (tool.kind == ToolKind::parameters)
        return command_payload(typed_response(client->read_parameters(), client->record()));
    if (tool.kind == ToolKind::screenshot)
        return command_payload(typed_response(client->capture_screenshot(), client->record()));
    if (tool.kind == ToolKind::set_parameter)
        return command_payload(typed_response(
            client->set_parameter_typed(*args.parameter_id, *args.value,
                                        args.normalized),
            client->record()));
    return command_payload(raw_response(
        client->request_controlled(std::string(tool.method),
                                   make_params(tool, args)),
        client->record()));
}

} // namespace

std::string live_inspector_tools_json() {
    std::string output;
    for (const auto& tool : kTools) {
        if (!output.empty()) output += ',';
        output += tool_json(tool);
    }
    return output;
}

std::optional<std::string> handle_live_inspector_tool(
    std::string_view name, const choc::value::ValueView& arguments) {
    const auto* tool = find_tool(name);
    if (!tool) return std::nullopt;
    Arguments parsed;
    if (const auto error = parse_arguments(*tool, arguments, parsed))
        return error_payload(*error,
                             error->find("selection") != std::string::npos ||
                                     error->starts_with("session_id") ||
                                     error->starts_with("instance_id") ||
                                     error->starts_with("publication_id")
                                 ? "invalid_selector"
                                 : "invalid_arguments");
    return execute(*tool, parsed);
}

} // namespace pulp_mcp
