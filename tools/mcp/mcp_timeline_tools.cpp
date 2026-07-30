#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "timeline_mcp_tools.h"
#include "timeline_session_store.hpp"

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
#include <vector>

namespace pulp_mcp {
namespace {

struct TimelineArguments {
    std::shared_ptr<const pulp::timeline::ParsedJson> parsed;
    const pulp::timeline::JsonValue* project = nullptr;
    const pulp::timeline::JsonValue* session_id = nullptr;
    const pulp::timeline::JsonValue* commands = nullptr;
    const pulp::timeline::JsonValue* output = nullptr;
    const pulp::timeline::JsonValue* sample_rate = nullptr;
    const pulp::timeline::JsonValue* input = nullptr;
    const pulp::timeline::JsonValue* format = nullptr;
    const pulp::timeline::JsonValue* accept_losses = nullptr;
    const pulp::timeline::JsonValue* plan_only = nullptr;
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
    result.session_id = root.find("session_id");
    result.commands = root.find("commands");
    result.output = root.find("output");
    result.sample_rate = root.find("sample_rate");
    result.input = root.find("input");
    result.format = root.find("format");
    result.accept_losses = root.find("accept_losses");
    result.plan_only = root.find("plan_only");
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
    auto opened = pulp::tools::timeline::project_open(timeline_project_source(*project));
    if (!opened)
        return timeline_result(std::move(opened));
    auto parsed = pulp::timeline::parse_json(opened.json);
    const auto* canonical = parsed ? parsed.value()->root().find("project") : nullptr;
    if (canonical == nullptr)
        return timeline_argument_error("Error: opened project did not contain canonical state");
    std::string error;
    auto session_id = open_timeline_session(parsed.value()->raw(*canonical), error);
    if (!session_id)
        return timeline_argument_error("Error: " + error);
    opened.json.insert(opened.json.size() - 1,
                       ",\"session_id\":" + pulp::timeline::quote_json_string(*session_id));
    return timeline_result(std::move(opened));
}

std::string handle_timeline_command_apply(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    const auto* session_id = required_timeline_string(arguments.value().session_id);
    if ((project == nullptr) == (session_id == nullptr))
        return timeline_argument_error("Error: exactly one of project or session_id is required");
    const auto* commands = arguments.value().commands;
    if (commands == nullptr || commands->kind != pulp::timeline::JsonValue::Kind::Array ||
        commands->array.empty())
        return timeline_argument_error("Error: commands must be a non-empty array");
    const auto commands_json = arguments.value().parsed->raw(*commands);
    if (session_id != nullptr)
        return timeline_result(apply_timeline_session(*session_id, commands_json));
    return timeline_result(
        pulp::tools::timeline::command_apply(timeline_project_source(*project), commands_json));
}

std::string handle_timeline_diff(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* session_id = required_timeline_string(arguments.value().session_id);
    if (session_id == nullptr)
        return timeline_argument_error("Error: session_id is required");
    return timeline_result(diff_timeline_session(*session_id));
}

std::string handle_timeline_undo(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* session_id = required_timeline_string(arguments.value().session_id);
    if (session_id == nullptr)
        return timeline_argument_error("Error: session_id is required");
    return timeline_result(undo_timeline_session(*session_id));
}

std::string handle_timeline_redo(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* session_id = required_timeline_string(arguments.value().session_id);
    if (session_id == nullptr)
        return timeline_argument_error("Error: session_id is required");
    return timeline_result(redo_timeline_session(*session_id));
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

std::string handle_timeline_export(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* project = required_timeline_string(arguments.value().project);
    const auto* format = required_timeline_string(arguments.value().format);
    if (project == nullptr || format == nullptr)
        return timeline_argument_error("Error: project and format are required");
    bool plan_only = false;
    if (const auto* value = arguments.value().plan_only) {
        if (value->kind != pulp::timeline::JsonValue::Kind::Boolean)
            return timeline_argument_error("Error: plan_only must be a boolean");
        plan_only = value->boolean;
    }
    if (plan_only) {
        if (arguments.value().output != nullptr)
            return timeline_argument_error("Error: output must be absent when plan_only is true");
        if (arguments.value().accept_losses != nullptr)
            return timeline_argument_error(
                "Error: accept_losses must be absent when plan_only is true");
        return timeline_result(pulp::tools::timeline::plan_export_project(
            timeline_project_source(*project), *format));
    }
    const auto* output = required_timeline_string(arguments.value().output);
    if (output == nullptr)
        return timeline_argument_error("Error: output is required when publishing an export");
    std::vector<std::string> accepted_losses;
    if (const auto* losses = arguments.value().accept_losses) {
        if (losses->kind != pulp::timeline::JsonValue::Kind::Array)
            return timeline_argument_error("Error: accept_losses must be an array");
        accepted_losses.reserve(losses->array.size());
        for (const auto& loss : losses->array) {
            if (loss.kind != pulp::timeline::JsonValue::Kind::String || loss.scalar.empty())
                return timeline_argument_error(
                    "Error: every accept_losses entry must be a concept id");
            accepted_losses.push_back(loss.scalar);
        }
    }
    return timeline_result(pulp::tools::timeline::export_project(
        timeline_project_source(*project), *format,
        pulp::tools::timeline::filesystem_path_from_utf8(*output), accepted_losses));
}

std::string handle_timeline_import(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* input = required_timeline_string(arguments.value().input);
    const auto* format = required_timeline_string(arguments.value().format);
    const auto* output = required_timeline_string(arguments.value().output);
    if (input == nullptr || format == nullptr || output == nullptr)
        return timeline_argument_error("Error: input, format, and output are required");
    return timeline_result(pulp::tools::timeline::import_project(
        pulp::tools::timeline::filesystem_path_from_utf8(*input), *format,
        pulp::tools::timeline::filesystem_path_from_utf8(*output)));
}

std::optional<std::string> handle_timeline_tool(std::string_view name,
                                                const std::string& params_json) {
    using Handler = std::string (*)(const std::string&);
    struct ToolBinding {
        std::string_view name;
        Handler handler;
    };
    static constexpr std::array<ToolBinding, 10> bindings{
        ToolBinding{kTimelineProjectOpenToolName, handle_timeline_project_open},
        ToolBinding{kTimelineCommandApplyToolName, handle_timeline_command_apply},
        ToolBinding{kTimelineDiffToolName, handle_timeline_diff},
        ToolBinding{kTimelineUndoToolName, handle_timeline_undo},
        ToolBinding{kTimelineRedoToolName, handle_timeline_redo},
        ToolBinding{kTimelineValidateToolName, handle_timeline_validate},
        ToolBinding{kTimelineExplainToolName, handle_timeline_explain},
        ToolBinding{kTimelineRenderToolName, handle_timeline_render},
        ToolBinding{kTimelineExportToolName, handle_timeline_export},
        ToolBinding{kTimelineImportToolName, handle_timeline_import},
    };
    static_assert(bindings.size() == kTimelineMcpToolNames.size());
    for (const auto& binding : bindings) {
        if (name == binding.name)
            return binding.handler(params_json);
    }
    return std::nullopt;
}

} // namespace pulp_mcp
