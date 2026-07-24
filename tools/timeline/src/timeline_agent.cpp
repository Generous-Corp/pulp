#include <pulp/tools/timeline/agent.hpp>

#include "timeline_agent_internal.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_codegen.hpp>
#include <pulp/timeline/serialize.hpp>

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace pulp::tools::timeline {

std::filesystem::path filesystem_path_from_utf8(std::string_view value) {
    std::u8string utf8(value.size(), u8'\0');
    if (!value.empty())
        std::memcpy(utf8.data(), value.data(), value.size());
    return std::filesystem::path(utf8);
}

std::string filesystem_path_to_utf8(const std::filesystem::path& value) {
    const auto utf8 = value.u8string();
    std::string result{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    if (!pulp::timeline::is_valid_utf8(result))
        throw std::invalid_argument("native path cannot be represented as UTF-8");
    return result;
}

namespace {

std::string error_json(std::string_view stage, std::string_view message,
                       std::string_view path = {}) {
    std::string result = "{\"error\":{\"message\":";
    result += pulp::timeline::quote_json_string(message);
    if (!path.empty()) {
        result += ",\"path\":";
        result += pulp::timeline::quote_json_string(path);
    }
    result += ",\"stage\":";
    result += pulp::timeline::quote_json_string(stage);
    result += "},\"ok\":false}";
    return result;
}

} // namespace

OperationResult detail::failure(std::string_view stage, std::string_view message,
                                std::string_view path, int exit_code) {
    return {exit_code, error_json(stage, message, path)};
}

std::string detail::persistence_message(const pulp::timeline::PersistenceError& error) {
    return "timeline persistence error " + std::to_string(static_cast<unsigned>(error.code));
}

using detail::failure;
using detail::persistence_message;

ProjectSource::ProjectSource(ProjectSourceKind kind, std::string text)
    : kind_(kind), text_(std::move(text)) {}

ProjectSource::ProjectSource(std::filesystem::path path)
    : kind_(ProjectSourceKind::File), file_path_(std::move(path)) {}

ProjectSource ProjectSource::auto_detect(std::string_view value) {
    return ProjectSource(ProjectSourceKind::AutoDetect, std::string(value));
}

ProjectSource ProjectSource::inline_json(std::string_view json) {
    return ProjectSource(ProjectSourceKind::InlineJson, std::string(json));
}

ProjectSource ProjectSource::file(const std::filesystem::path& path) {
    return ProjectSource(path);
}

OperationResult project_open(const ProjectSource& project) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto serialized = pulp::timeline::serialize_project(loaded.value().value, registry.value());
    if (!serialized)
        return failure("open", persistence_message(serialized.error()), serialized.error().path);
    return {0, "{\"ok\":true,\"project\":" + serialized.value().json + "}"};
}

OperationResult command_apply(const ProjectSource& project, std::string_view commands) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto decoded = pulp::timeline::deserialize_commands(commands, registry.value());
    if (!decoded)
        return failure("apply", persistence_message(decoded.error()), decoded.error().path, 2);
    auto session = pulp::timeline::DocumentSession::create(std::move(loaded).value().value);
    if (!session)
        return failure("apply", "could not create a document session");
    auto writer = session.value()->register_writer();
    if (!writer)
        return failure("apply", "could not register a document writer");
    pulp::timeline::Transaction transaction;
    transaction.id = writer.value().allocate_transaction_id();
    transaction.expected_revision = session.value()->revision();
    transaction.commands.reserve(decoded.value().size());
    for (auto& command : decoded.value())
        transaction.commands.push_back({writer.value().allocate_command_id(), std::move(command)});
    auto committed = session.value()->submit(writer.value(), std::move(transaction));
    if (!committed) {
        const auto& error = committed.error();
        return failure("apply",
                       "timeline transaction conflict " +
                           std::to_string(static_cast<unsigned>(error.code)),
                       error.item.valid() ? std::to_string(error.item.value) : std::string{});
    }
    auto serialized =
        pulp::timeline::serialize_project(*committed.value().snapshot, registry.value());
    if (!serialized)
        return failure("apply", persistence_message(serialized.error()), serialized.error().path);
    return {0, "{\"ok\":true,\"project\":" + serialized.value().json + ",\"revision\":\"" +
                   std::to_string(committed.value().revision.value) + "\"}"};
}

OperationResult validate(const ProjectSource& project) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    return {0, "{\"diagnostics\":[],\"ok\":true}"};
}

OperationResult explain(const ProjectSource& project, std::uint32_t sample_rate) {
    if (sample_rate == 0 || sample_rate > timebase::kMaximumCompiledSampleRate)
        return failure("explain", "sample_rate must be between 1 and 768000", {}, 2);
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto compiled = detail::compile_project(loaded.value(), sample_rate);
    if (!compiled)
        return failure("explain", detail::compile_error_message(compiled.error()));
    auto program = compiled.value()->store.read();
    if (!program)
        return failure("explain", "compiled program was not published");

    std::string json = "{\"generation\":\"" + std::to_string(program->generation()) +
                       "\",\"ok\":true,\"project_id\":\"" +
                       std::to_string(program->project_id().value) + "\",\"sequence_id\":\"" +
                       std::to_string(program->sequence_id().value) + "\",\"tracks\":[";
    bool first_track = true;
    for (const auto& track : program->tracks()) {
        if (!first_track)
            json += ",";
        first_track = false;
        json += "{\"audio_regions\":";
        json += std::to_string(track->audio_program() ? track->audio_program()->clips().size() : 0);
        json += ",\"automation\":";
        json += track->automation_program() ? "true" : "false";
        json += ",\"clip_ids\":[";
        bool first_clip = true;
        for (const auto id : track->ordered_clip_ids()) {
            if (!first_clip)
                json += ",";
            first_clip = false;
            json += "\"" + std::to_string(id.value) + "\"";
        }
        json += "],\"note_events\":";
        json += std::to_string(track->arrangement_note_events().size());
        json += ",\"pdc_offset_samples\":null,\"track_id\":\"";
        json += std::to_string(track->id().value);
        json += "\"}";
    }
    json += "]}";
    return {0, std::move(json)};
}

OperationResult project_open(std::string_view project) {
    return project_open(ProjectSource::auto_detect(project));
}

OperationResult command_apply(std::string_view project, std::string_view commands) {
    return command_apply(ProjectSource::auto_detect(project), commands);
}

OperationResult validate(std::string_view project) {
    return validate(ProjectSource::auto_detect(project));
}

OperationResult explain(std::string_view project, std::uint32_t sample_rate) {
    return explain(ProjectSource::auto_detect(project), sample_rate);
}

OperationResult render(std::string_view project, const std::filesystem::path& output,
                       std::uint32_t sample_rate) {
    return render(ProjectSource::auto_detect(project), output, sample_rate);
}

OperationResult schema() {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto manifest = pulp::timeline::emit_schema_manifest(registry.value());
    if (!manifest)
        return failure("schema", persistence_message(manifest.error()), manifest.error().path);
    return {0, std::move(manifest).value()};
}

} // namespace pulp::tools::timeline
