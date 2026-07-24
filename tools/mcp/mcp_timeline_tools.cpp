#include "mcp_tools.hpp"

#include "mcp_json.hpp"

#include <pulp/tools/timeline/agent.hpp>

#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/schema_json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace pulp_mcp {
namespace {

struct TimelineArguments {
    std::shared_ptr<const pulp::timeline::ParsedJson> parsed;
    const pulp::timeline::JsonValue* project = nullptr;
    const pulp::timeline::JsonValue* commands = nullptr;
    const pulp::timeline::JsonValue* output = nullptr;
    const pulp::timeline::JsonValue* sample_rate = nullptr;
};

pulp::runtime::Result<TimelineArguments, std::string>
parse_timeline_arguments(const std::string& params_json) {
    auto parsed = pulp::timeline::parse_json(params_json);
    if (!parsed)
        return pulp::runtime::Err(std::string("Error: arguments must be valid JSON"));
    if (parsed.value()->root().kind != pulp::timeline::JsonValue::Kind::Object)
        return pulp::runtime::Err(std::string("Error: arguments must be an object"));
    const auto& root = parsed.value()->root();
    TimelineArguments result;
    result.parsed = std::move(parsed).value();
    result.project = root.find("project");
    result.commands = root.find("commands");
    result.output = root.find("output");
    result.sample_rate = root.find("sample_rate");
    return pulp::runtime::Ok(std::move(result));
}

std::string timeline_argument_error(std::string_view message) {
    auto payload =
        json_tool_payload("{\"error\":{\"message\":" + pulp::timeline::quote_json_string(message) +
                          ",\"stage\":\"arguments\"},\"ok\":false}");
    payload.insert(payload.size() - 1, ",\"isError\":true");
    return payload;
}

const std::string* required_timeline_string(const pulp::timeline::JsonValue* value) {
    if (value == nullptr || value->kind != pulp::timeline::JsonValue::Kind::String ||
        value->scalar.empty())
        return nullptr;
    return &value->scalar;
}

pulp::runtime::Result<std::uint32_t, std::string>
timeline_sample_rate(const pulp::timeline::JsonValue* value) {
    if (value == nullptr)
        return pulp::runtime::Ok(std::uint32_t{48'000});
    auto parsed = pulp::timeline::parse_u32_number(*value, "sample_rate");
    if (!parsed || parsed.value() == 0 ||
        parsed.value() > pulp::timebase::kMaximumCompiledSampleRate) {
        return pulp::runtime::Err(
            std::string("Error: sample_rate must be an integer between 1 and 768000"));
    }
    return pulp::runtime::Ok(parsed.value());
}

std::string timeline_result(pulp::tools::timeline::OperationResult result) {
    auto payload = json_tool_payload(result.json);
    if (!result)
        payload.insert(payload.size() - 1, ",\"isError\":true");
    return payload;
}

pulp::tools::timeline::ProjectSource timeline_project_source(std::string_view value) {
    auto path = pulp::tools::timeline::filesystem_path_from_utf8(value);
    std::error_code error;
    if (std::filesystem::exists(path, error))
        return pulp::tools::timeline::ProjectSource::file(path);
    if (pulp::timeline::parse_json(value))
        return pulp::tools::timeline::ProjectSource::inline_json(value);
    return pulp::tools::timeline::ProjectSource::file(path);
}

} // namespace

std::string handle_timeline_project_open(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    if (project == nullptr)
        return timeline_argument_error("Error: project is required");
    return timeline_result(pulp::tools::timeline::project_open(timeline_project_source(*project)));
}

std::string handle_timeline_command_apply(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    if (project == nullptr)
        return timeline_argument_error("Error: project is required");
    const auto* commands = arguments.value().commands;
    if (commands == nullptr || commands->kind != pulp::timeline::JsonValue::Kind::Array ||
        commands->array.empty())
        return timeline_argument_error("Error: commands must be a non-empty array");
    return timeline_result(pulp::tools::timeline::command_apply(
        timeline_project_source(*project), arguments.value().parsed->raw(*commands)));
}

std::string handle_timeline_validate(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    if (project == nullptr)
        return timeline_argument_error("Error: project is required");
    return timeline_result(pulp::tools::timeline::validate(timeline_project_source(*project)));
}

std::string handle_timeline_explain(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    if (project == nullptr)
        return timeline_argument_error("Error: project is required");
    auto sample_rate = timeline_sample_rate(arguments.value().sample_rate);
    if (!sample_rate)
        return timeline_argument_error(sample_rate.error());
    return timeline_result(
        pulp::tools::timeline::explain(timeline_project_source(*project), sample_rate.value()));
}

std::string handle_timeline_render(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    const auto* output = required_timeline_string(arguments.value().output);
    if (project == nullptr || output == nullptr)
        return timeline_argument_error("Error: project and output are required");
    auto sample_rate = timeline_sample_rate(arguments.value().sample_rate);
    if (!sample_rate)
        return timeline_argument_error(sample_rate.error());
    return timeline_result(pulp::tools::timeline::render(
        timeline_project_source(*project),
        pulp::tools::timeline::filesystem_path_from_utf8(*output), sample_rate.value()));
}

} // namespace pulp_mcp
