#include "mcp_tools.hpp"

#include "mcp_json.hpp"
#include "timeline_mcp_tools.h"
#include "timeline_session_store.hpp"

#include <pulp/tools/timeline/agent.hpp>
#include <pulp/tools/timeline/writer_profile.hpp>

#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/schema_json.hpp>

#include <charconv>
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
    const pulp::timeline::JsonValue* writer_profile = nullptr;
    const pulp::timeline::JsonValue* idempotency_key = nullptr;
    const pulp::timeline::JsonValue* expected_revision = nullptr;
    const pulp::timeline::JsonValue* sequence_id = nullptr;
    const pulp::timeline::JsonValue* start = nullptr;
    const pulp::timeline::JsonValue* end = nullptr;
    const pulp::timeline::JsonValue* limit = nullptr;
    const pulp::timeline::JsonValue* absolute = nullptr;
    const pulp::timeline::JsonValue* after = nullptr;
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
    result.writer_profile = root.find("writer_profile");
    result.idempotency_key = root.find("idempotency_key");
    result.expected_revision = root.find("expected_revision");
    result.sequence_id = root.find("sequence_id");
    result.start = root.find("start");
    result.end = root.find("end");
    result.limit = root.find("limit");
    result.absolute = root.find("absolute");
    result.after = root.find("after");
    return pulp::runtime::Ok(std::move(result));
}

std::string timeline_argument_error(std::string_view message) {
    auto payload =
        json_tool_payload("{\"error\":{\"message\":" + pulp::timeline::quote_json_string(message) +
                          ",\"stage\":\"arguments\"},\"ok\":false}");
    payload.insert(payload.size() - 1, ",\"isError\":true");
    return payload;
}

/// Resolves the writer authority a caller selected for this call.
///
/// Absent selection is the non-destructive proposal authority. An unrecognized
/// name is a usage error rather than a fallback to a more permissive profile.
pulp::runtime::Result<pulp::tools::timeline::WriterProfile, std::string>
timeline_writer_profile(const pulp::timeline::JsonValue* value) {
    if (value == nullptr)
        return pulp::runtime::Ok(pulp::tools::timeline::proposal_writer_profile());
    if (value->kind != pulp::timeline::JsonValue::Kind::String)
        return pulp::runtime::Err(std::string("Error: writer_profile must be a string"));
    auto profile = pulp::tools::timeline::writer_profile_by_name(value->scalar);
    if (!profile)
        return pulp::runtime::Err(
            "Error: unknown writer_profile \"" + value->scalar + "\"; expected one of " +
            std::string(pulp::tools::timeline::selectable_writer_profile_names()));
    return pulp::runtime::Ok(std::move(profile).value());
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

/// Reads the retry and staleness controls an apply may carry.
///
/// Both are optional, and both are refused rather than ignored when the shapes
/// are wrong: a caller that mistypes a retry token would otherwise be told its
/// call was deduplicated when nothing recorded it.
pulp::runtime::Result<TimelineApplyOptions, std::string>
timeline_apply_options(const TimelineArguments& arguments) {
    TimelineApplyOptions options;
    if (const auto* key = arguments.idempotency_key; key != nullptr) {
        if (key->kind != pulp::timeline::JsonValue::Kind::String || key->scalar.empty())
            return pulp::runtime::Err(
                std::string("Error: idempotency_key must be a non-empty string"));
        options.idempotency_key = key->scalar;
    }
    if (const auto* revision = arguments.expected_revision; revision != nullptr) {
        if (revision->kind != pulp::timeline::JsonValue::Kind::Number ||
            revision->scalar.empty() || revision->scalar.front() == '-' ||
            revision->scalar.find_first_of(".eE") != std::string::npos)
            return pulp::runtime::Err(
                std::string("Error: expected_revision must be a non-negative integer"));
        std::uint64_t value = 0;
        const auto* first = revision->scalar.data();
        const auto* last = first + revision->scalar.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last)
            return pulp::runtime::Err(
                std::string("Error: expected_revision must be a non-negative integer"));
        options.expected_revision = pulp::timeline::DocumentRevision{value};
    }
    return pulp::runtime::Ok(std::move(options));
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
    auto profile = timeline_writer_profile(arguments.value().writer_profile);
    if (!profile)
        return timeline_argument_error(profile.error());
    std::string error;
    auto session_id =
        open_timeline_session(parsed.value()->raw(*canonical), profile.value(), error);
    if (!session_id)
        return timeline_argument_error("Error: " + error);
    // The store charged this exact payload against its output limit, so the
    // opened session reports the authority it was admitted under rather than a
    // second construction that could drift from the accounted one.
    opened.json = timeline_session_open_response(parsed.value()->raw(*canonical), *session_id,
                                                 profile.value());
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
    auto options = timeline_apply_options(arguments.value());
    if (!options)
        return timeline_argument_error(options.error());
    if (session_id != nullptr)
        return timeline_result(
            apply_timeline_session(*session_id, commands_json, options.value()));
    // Both controls are session state: a stateless apply opens its own document
    // and keeps no retry record, so honouring either here would report a
    // deduplication or a staleness check that nothing performed.
    if (!options.value().idempotency_key.empty() || options.value().expected_revision)
        return timeline_argument_error(
            "Error: idempotency_key and expected_revision require session_id");
    auto profile = timeline_writer_profile(arguments.value().writer_profile);
    if (!profile)
        return timeline_argument_error(profile.error());
    return timeline_result(pulp::tools::timeline::command_apply(
        timeline_project_source(*project), commands_json, profile.value()));
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

namespace {

/// Parses a JSON integer field into `out`, refusing any non-integer spelling.
///
/// A window bound arrives as a JSON number, but a number that carries a
/// fraction or an exponent is not a tick position; accepting one would silently
/// truncate a caller's request rather than tell them it was malformed.
template <typename T>
bool parse_view_integer(const pulp::timeline::JsonValue& value, T& out) {
    if (value.kind != pulp::timeline::JsonValue::Kind::Number || value.scalar.empty() ||
        value.scalar.find_first_of(".eE") != std::string::npos)
        return false;
    const auto* first = value.scalar.data();
    const auto* last = first + value.scalar.size();
    const auto parsed = std::from_chars(first, last, out);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

/// Reads the bounded-window arguments a `view_region` call carries.
pulp::runtime::Result<pulp::tools::timeline::RegionViewOptions, std::string>
timeline_region_options(const TimelineArguments& arguments) {
    pulp::tools::timeline::RegionViewOptions options;
    if (arguments.sequence_id == nullptr ||
        !parse_view_integer(*arguments.sequence_id, options.sequence_id) ||
        options.sequence_id == 0)
        return pulp::runtime::Err(
            std::string("Error: sequence_id must be a positive integer naming a sequence"));
    if (arguments.start == nullptr || !parse_view_integer(*arguments.start, options.start))
        return pulp::runtime::Err(std::string("Error: start must be a whole number of ticks"));
    if (arguments.end == nullptr || !parse_view_integer(*arguments.end, options.end))
        return pulp::runtime::Err(std::string("Error: end must be a whole number of ticks"));
    if (const auto* limit = arguments.limit) {
        if (!parse_view_integer(*limit, options.limit) || options.limit == 0)
            return pulp::runtime::Err(std::string("Error: limit must be a positive integer"));
    }
    if (const auto* absolute = arguments.absolute) {
        if (absolute->kind != pulp::timeline::JsonValue::Kind::Boolean)
            return pulp::runtime::Err(std::string("Error: absolute must be a boolean"));
        options.absolute = absolute->boolean;
    }
    if (const auto* after = arguments.after) {
        if (after->kind != pulp::timeline::JsonValue::Kind::String || after->scalar.empty())
            return pulp::runtime::Err(
                std::string("Error: after must be a continuation token from a prior page"));
        options.after = after->scalar;
    }
    return pulp::runtime::Ok(std::move(options));
}

} // namespace

std::string handle_timeline_view_outline(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* session = required_timeline_string(arguments.value().session_id);
    if (session == nullptr)
        return timeline_argument_error("Error: session_id is required");
    return timeline_result(view_outline_timeline_session(*session));
}

std::string handle_timeline_view_region(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* session = required_timeline_string(arguments.value().session_id);
    if (session == nullptr)
        return timeline_argument_error("Error: session_id is required");
    auto options = timeline_region_options(arguments.value());
    if (!options)
        return timeline_argument_error(options.error());
    return timeline_result(view_region_timeline_session(*session, options.value()));
}

std::string handle_timeline_view_diff(const std::string& params_json) {
    auto arguments = parse_timeline_arguments(params_json);
    if (!arguments)
        return timeline_argument_error(arguments.error());
    const auto* session = required_timeline_string(arguments.value().session_id);
    if (session == nullptr)
        return timeline_argument_error("Error: session_id is required");
    return timeline_result(view_diff_timeline_session(*session));
}

std::string timeline_view_mcp_tools_json_fragment() {
    // Hand-written rather than generated: the generated timeline catalog is
    // pinned to exactly the ten document-editing tools, and these three are a
    // read projection over that document rather than another editing verb.
    return
        R"JSON({"description":"Project a timeline session's document as a bounded, versioned outline.","inputSchema":{"additionalProperties":false,"properties":{"session_id":{"description":"Session identifier returned by pulp_timeline_project_open.","minLength":1,"type":"string"}},"required":["session_id"],"type":"object"},"name":"pulp_timeline_view_outline"},)JSON"
        R"JSON({"description":"Project one bounded window of clips from a sequence in a timeline session.","inputSchema":{"additionalProperties":false,"properties":{"absolute":{"description":"Read the absolute timebase instead of the musical one.","type":"boolean"},"after":{"description":"Continuation token from a prior page's next field, verbatim.","minLength":1,"type":"string"},"end":{"description":"Exclusive end of the half-open window, in the selected timebase.","type":"integer"},"limit":{"description":"Maximum clips per page. Clamped by the view's own limits.","minimum":1,"type":"integer"},"sequence_id":{"description":"Identifier of the sequence to page through.","minimum":1,"type":"integer"},"session_id":{"description":"Session identifier returned by pulp_timeline_project_open.","minLength":1,"type":"string"},"start":{"description":"Inclusive start of the half-open window, in the selected timebase.","type":"integer"}},"required":["session_id","sequence_id","start","end"],"type":"object"},"name":"pulp_timeline_view_region"},)JSON"
        R"JSON({"description":"Project the outline diff for a timeline session's most recent applied transaction.","inputSchema":{"additionalProperties":false,"properties":{"session_id":{"description":"Session identifier returned by pulp_timeline_project_open.","minLength":1,"type":"string"}},"required":["session_id"],"type":"object"},"name":"pulp_timeline_view_diff"})JSON";
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
    // The read-projection verbs are bound separately because the generated
    // catalog above is pinned to the ten editing tools it describes.
    static constexpr std::array<ToolBinding, 3> view_bindings{
        ToolBinding{"pulp_timeline_view_outline", handle_timeline_view_outline},
        ToolBinding{"pulp_timeline_view_region", handle_timeline_view_region},
        ToolBinding{"pulp_timeline_view_diff", handle_timeline_view_diff},
    };
    for (const auto& binding : bindings) {
        if (name == binding.name)
            return binding.handler(params_json);
    }
    for (const auto& binding : view_bindings) {
        if (name == binding.name)
            return binding.handler(params_json);
    }
    return std::nullopt;
}

} // namespace pulp_mcp
