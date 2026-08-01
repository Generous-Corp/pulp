#include "mcp_inspector_tools.hpp"

#include "mcp_json.hpp"
#include "mcp_tools.hpp"

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

using ToolDefinition = InspectorMcpToolDescriptor;
using ToolKind = InspectorMcpToolDescriptor::Kind;

const ToolDefinition* find_tool(std::string_view name) {
    return find_inspector_mcp_tool(name);
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
    std::optional<std::string> kind;
    std::optional<std::int64_t> channel;
    std::optional<std::int64_t> note;
    std::optional<std::int64_t> velocity;
    std::optional<bool> playing;
    std::optional<std::int64_t> position_samples;
    std::optional<double> tempo_bpm;
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
    if (tool.kind == ToolKind::inject_midi ||
        tool.kind == ToolKind::set_transport) {
        auto field_flag = [] (std::string_view name) -> std::uint32_t {
            if (name == "kind") return inspector_field_kind;
            if (name == "channel") return inspector_field_channel;
            if (name == "note") return inspector_field_note;
            if (name == "velocity") return inspector_field_velocity;
            if (name == "playing") return inspector_field_playing;
            if (name == "position_samples") return inspector_field_position_samples;
            if (name == "tempo_bpm") return inspector_field_tempo_bpm;
            return inspector_field_none;
        };
        for (std::uint32_t index = 0; index < object.size(); ++index) {
            const auto name = std::string_view(object.getObjectMemberAt(index).name);
            if (name == "session_id" || name == "instance_id" ||
                name == "publication_id")
                continue;
            const auto flag = field_flag(name);
            if (flag == inspector_field_none || (allowed & flag) == 0)
                return "unknown test-input field: " + std::string(name);
        }
    }
    if ((allowed & inspector_field_id) != 0) {
        std::optional<std::int64_t> id;
        if (auto error = read_integer(object, "id", (required & inspector_field_id) != 0,
                                      id, 0, 4294967295LL)) return error;
        if (id) output.parameter_id = static_cast<std::uint32_t>(*id);
    }
    if ((allowed & inspector_field_value) != 0) {
        if (!object.hasObjectMember("value")) {
            if ((required & inspector_field_value) != 0) return "value is required";
        } else {
            const auto value = object["value"];
            if (!value.isInt() && !value.isFloat())
                return "value must be a number";
            const auto number = value.isInt()
                ? static_cast<double>(value.getInt64())
                : value.getFloat64();
            if (!std::isfinite(number))
                return "value must be finite";
            output.value = number;
        }
    }
    if ((allowed & inspector_field_normalized) != 0 &&
        object.hasObjectMember("normalized")) {
        const auto value = object["normalized"];
        if (!value.isBool()) return "normalized must be a boolean";
        output.normalized = value.getBool();
    }
    if ((allowed & inspector_field_expression) != 0) {
        if (auto error = read_string(
                object, "expression",
                (required & inspector_field_expression) != 0, output.expression))
            return error;
        if ((required & inspector_field_expression) != 0 && output.expression->empty())
            return "expression must not be empty";
    }
    if ((allowed & inspector_field_view_name) != 0)
        if (auto error = read_string(object, "view_name", (required & inspector_field_view_name) != 0, output.view_name)) return error;
    if ((allowed & inspector_field_metrics) != 0) {
        if (!object.hasObjectMember("metrics")) {
            if ((required & inspector_field_metrics) != 0) return "metrics is required";
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
    if ((allowed & inspector_field_fps) != 0)
        if (auto error = read_integer(object, "fps", false, output.fps, 1)) return error;
    if ((allowed & inspector_field_trace_id) != 0)
        if (auto error = read_integer(object, "trace_id", (required & inspector_field_trace_id) != 0, output.trace_id, 0)) return error;
    if ((allowed & inspector_field_frame) != 0)
        if (auto error = read_integer(object, "frame", (required & inspector_field_frame) != 0, output.frame, 0)) return error;
    if ((allowed & inspector_field_categories) != 0) {
        if (object.hasObjectMember("categories")) {
            const auto categories = object["categories"];
            if (!categories.isArray()) return "categories must be an array";
            for (std::uint32_t index = 0; index < categories.size(); ++index)
                if (!categories[index].isString())
                    return "categories entries must be strings";
            output.categories_json = choc::json::toString(categories, false);
        }
    }
    if ((allowed & inspector_field_ring_mb) != 0)
        if (auto error = read_integer(object, "ring_mb", false, output.ring_mb, 1, 512)) return error;
    if ((allowed & inspector_field_sql) != 0)
        if (auto error = read_string(object, "sql", false, output.sql)) return error;
    if ((allowed & inspector_field_preset) != 0)
        if (auto error = read_string(object, "preset", false, output.preset)) return error;
    if ((allowed & inspector_field_format) != 0) {
        if (auto error = read_string(object, "format", false, output.format)) return error;
        if (output.format && *output.format != "json" &&
            *output.format != "table" && *output.format != "csv")
            return "format must be json, table, or csv";
    }
    if ((allowed & inspector_field_question) != 0)
        if (auto error = read_string(object, "question", (required & inspector_field_question) != 0, output.question)) return error;
    if ((allowed & inspector_field_kind) != 0) {
        if (auto error = read_string(object, "kind", (required & inspector_field_kind) != 0,
                                     output.kind)) return error;
        if (output.kind && *output.kind != "note_on" && *output.kind != "note_off")
            return "kind must be note_on or note_off";
    }
    if ((allowed & inspector_field_channel) != 0)
        if (auto error = read_integer(object, "channel", (required & inspector_field_channel) != 0,
                                      output.channel, 1, 16)) return error;
    if ((allowed & inspector_field_note) != 0)
        if (auto error = read_integer(object, "note", (required & inspector_field_note) != 0,
                                      output.note, 0, 127)) return error;
    if ((allowed & inspector_field_velocity) != 0)
        if (auto error = read_integer(object, "velocity", false, output.velocity, 0, 127))
            return error;
    if (tool.kind == ToolKind::inject_midi && output.kind &&
        *output.kind == "note_on" && !output.velocity)
        return "velocity is required for note_on";
    if ((allowed & inspector_field_playing) != 0 &&
        object.hasObjectMember("playing")) {
        if (!object["playing"].isBool()) return "playing must be a boolean";
        output.playing = object["playing"].getBool();
    }
    if ((allowed & inspector_field_position_samples) != 0)
        if (auto error = read_integer(object, "position_samples", false,
                                      output.position_samples, 0)) return error;
    if ((allowed & inspector_field_tempo_bpm) != 0 &&
        object.hasObjectMember("tempo_bpm")) {
        const auto value = object["tempo_bpm"];
        if (!value.isInt() && !value.isFloat())
            return "tempo_bpm must be a number";
        const auto number = value.isInt()
            ? static_cast<double>(value.getInt64())
            : value.getFloat64();
        if (!std::isfinite(number) || number < 20.0 || number > 400.0)
            return "tempo_bpm must be finite and from 20 to 400";
        output.tempo_bpm = number;
    }
    if (tool.kind == ToolKind::set_transport && !output.playing &&
        !output.position_samples && !output.tempo_bpm)
        return "set transport requires playing, position_samples, or tempo_bpm";
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
    if (tool.kind == ToolKind::inject_midi) {
        pulp::inspect::MidiTestInput input;
        input.kind = *args.kind == "note_on"
                         ? pulp::inspect::MidiTestInputKind::NoteOn
                         : pulp::inspect::MidiTestInputKind::NoteOff;
        input.channel = static_cast<std::uint8_t>(*args.channel - 1);
        input.note = static_cast<std::uint8_t>(*args.note);
        input.velocity = static_cast<std::uint8_t>(args.velocity.value_or(0));
        return command_payload(typed_response(
            client->inject_midi_typed(input), client->record()));
    }
    if (tool.kind == ToolKind::set_transport)
        return command_payload(typed_response(
            client->set_transport_typed(
                {.playing = args.playing,
                 .position_samples = args.position_samples,
                 .tempo_bpm = args.tempo_bpm}),
            client->record()));
    return command_payload(raw_response(
        client->request_controlled(std::string(tool.method),
                                   make_params(tool, args)),
        client->record()));
}

} // namespace


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
