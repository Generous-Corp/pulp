#include <pulp/tools/timeline/agent.hpp>

#include "atomic_publisher.hpp"
#include "bounded_zip_archive.hpp"
#include "dawproject_media_packager.hpp"
#include "timeline_agent_internal.hpp"

#include <pulp/dawproject/dawproject_export.hpp>
#include <pulp/interchange/export_plan.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/url.hpp>
#include <pulp/smf/interchange.hpp>
#include <pulp/timeline/asset_path.hpp>
#include <pulp/timeline/dawproject_import.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/smf.hpp>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pulp::tools::timeline {
namespace {

namespace fs = std::filesystem;
using pulp::interchange::Concept;
using pulp::interchange::Format;

std::optional<Format> parse_format(std::string_view value) noexcept {
    if (value == "smf")
        return Format::Smf;
    if (value == "dawproject")
        return Format::DawProject;
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> read_file_bounded(const fs::path& path,
                                                           std::uint64_t maximum_bytes) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum_bytes || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        return std::nullopt;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return std::nullopt;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                           static_cast<std::streamsize>(bytes.size())))
            return std::nullopt;
        if (stream.peek() != std::ifstream::traits_type::eof())
            return std::nullopt;
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}

std::string output_json(std::string_view format, const fs::path& output,
                        std::string_view extra = {}) {
    std::string result =
        "{\"format\":" + pulp::timeline::quote_json_string(format) + ",\"ok\":true,\"output\":";
    result += pulp::timeline::quote_json_string(filesystem_path_to_utf8(output));
    if (!extra.empty()) {
        result += ',';
        result += extra;
    }
    result += '}';
    return result;
}

std::string concept_array_json(std::span<const Concept> concepts) {
    std::string result = "[";
    bool first = true;
    for (const auto concept_value : concepts) {
        if (!first)
            result += ',';
        first = false;
        result += pulp::timeline::quote_json_string(pulp::interchange::concept_id(concept_value));
    }
    result += ']';
    return result;
}

std::string export_result_json(std::string_view format, const fs::path& output,
                               const pulp::interchange::ExportPlan& plan) {
    std::string result =
        "{\"format\":" + pulp::timeline::quote_json_string(format) +
        ",\"lossless\":" + (plan.is_lossless() ? std::string("true") : std::string("false")) +
        ",\"manifest\":" + pulp::interchange::loss_manifest_json(plan) + ",\"ok\":true";
    result += ",\"output\":";
    result += pulp::timeline::quote_json_string(filesystem_path_to_utf8(output));
    result += ",\"plan_only\":false,\"required_consent\":[]";
    result += '}';
    return result;
}

std::string export_plan_json(std::string_view format, const pulp::interchange::ExportPlan& plan) {
    const auto required = plan.required_consent();
    std::string result =
        "{\"format\":" + pulp::timeline::quote_json_string(format) +
        ",\"lossless\":" + (plan.is_lossless() ? std::string("true") : std::string("false")) +
        ",\"manifest\":" + pulp::interchange::loss_manifest_json(plan) +
        ",\"ok\":true,\"plan_only\":true,\"required_consent\":";
    result += concept_array_json(required);
    result += '}';
    return result;
}

OperationResult export_error_result(std::string_view format,
                                    const pulp::interchange::ExportPlan& plan,
                                    std::string_view stage, std::string_view message,
                                    std::string_view path = {},
                                    std::span<const Concept> required_consent = {}) {
    std::string result = "{\"error\":{\"message\":";
    result += pulp::timeline::quote_json_string(message);
    if (!path.empty()) {
        result += ",\"path\":";
        result += pulp::timeline::quote_json_string(path);
    }
    result += ",\"stage\":";
    result += pulp::timeline::quote_json_string(stage);
    result += "},\"format\":";
    result += pulp::timeline::quote_json_string(format);
    result += ",\"manifest\":";
    result += pulp::interchange::loss_manifest_json(plan);
    result += ",\"ok\":false,\"required_consent\":";
    result += concept_array_json(required_consent);
    result += '}';
    return {1, std::move(result)};
}

} // namespace

OperationResult plan_export_project(const ProjectSource& project, std::string_view format_text) {
    const auto format = parse_format(format_text);
    if (!format)
        return detail::failure("arguments", "format (smf or dawproject) is required", {}, 2);

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return detail::failure("open", detail::persistence_message(loaded.error()),
                               loaded.error().path);
    const auto plan = pulp::interchange::plan_export(loaded.value().value, *format);
    return {0, export_plan_json(format_text, plan)};
}

OperationResult export_project(const ProjectSource& project, std::string_view format_text,
                               const fs::path& output_directory,
                               const std::vector<std::string>& accepted_loss_names) {
    const auto format = parse_format(format_text);
    if (!format || output_directory.empty())
        return detail::failure("arguments", "format (smf or dawproject) and output are required",
                               {}, 2);
    if (*format == Format::DawProject && output_directory.extension() != ".dawproject")
        return detail::failure("arguments", "DAWproject output must end in .dawproject",
                               filesystem_path_to_utf8(output_directory), 2);

    pulp::interchange::ExportOptions options;
    std::set<Concept> unique_losses;
    for (const auto& name : accepted_loss_names) {
        const auto concept_value = pulp::interchange::concept_from_id(name);
        if (concept_value == Concept::Unknown)
            return detail::failure("arguments", "unknown loss concept: " + name, {}, 2);
        if (unique_losses.insert(concept_value).second)
            options.accepted_losses.push_back(concept_value);
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return detail::failure("open", detail::persistence_message(loaded.error()),
                               loaded.error().path);
    const auto plan = pulp::interchange::plan_export(loaded.value().value, *format);
    auto exported = pulp::interchange::run_export(
        plan, options, *format == Format::Smf ? pulp::smf::writer() : pulp::dawproject::writer());
    if (!exported) {
        const auto path = exported.error().concepts.empty()
                              ? std::string_view{}
                              : pulp::interchange::concept_id(exported.error().concepts.front());
        return export_error_result(format_text, plan, "export", exported.error().message, path,
                                   exported.error().concepts);
    }
    if (*format == Format::DawProject) {
        const auto existing_charge = detail::retained_external_artifact_reserve(exported.value());
        if (!existing_charge ||
            *existing_charge > detail::kMaxAssetWorkingSetBytes - detail::kZipStdioReserveBytes)
            return export_error_result(format_text, plan, "export",
                                       "DAWproject export exceeds the working-set limit");
        const auto media_budget =
            detail::kMaxAssetWorkingSetBytes - detail::kZipStdioReserveBytes - *existing_charge;
        auto media_result =
            detail::add_dawproject_media(loaded.value(), exported.value(), media_budget);
        if (!media_result) {
            const auto& error = media_result.error();
            const auto asset = error.asset_name.empty()
                                   ? std::string("asset id ") + std::to_string(error.asset_id)
                                   : std::string("asset '") + error.asset_name + "'";
            return export_error_result(
                format_text, plan, "export",
                "could not add DAWproject media " + asset + ": " + error.reason, error.asset_name);
        }
        auto archive = detail::write_dawproject_archive_no_replace(
            exported.value(), output_directory, detail::kMaxAssetWorkingSetBytes);
        if (!archive)
            return export_error_result(
                format_text, plan,
                archive.error().code == detail::DawProjectArchiveErrorCode::Publish ? "publish"
                                                                                    : "export",
                archive.error().message, filesystem_path_to_utf8(output_directory));
        return {0, export_result_json(format_text, output_directory, plan)};
    }

    auto publisher = detail::AtomicPublisher::create(output_directory);
    if (!publisher)
        return export_error_result(format_text, plan, "publish",
                                   "output directory must not exist and its parent must exist",
                                   filesystem_path_to_utf8(output_directory));
    for (const auto& artifact : exported.value().artifacts)
        if (!publisher->write(artifact.name, artifact.bytes))
            return export_error_result(format_text, plan, "publish",
                                       "could not stage export artifact", artifact.name);
    if (!publisher->commit_directory())
        return export_error_result(format_text, plan, "publish",
                                   "output directory appeared before atomic publication",
                                   filesystem_path_to_utf8(output_directory));
    return {0, export_result_json(format_text, output_directory, plan)};
}

OperationResult import_project(const fs::path& input, std::string_view format_text,
                               const fs::path& output_directory) {
    const auto format = parse_format(format_text);
    if (!format || input.empty() || output_directory.empty())
        return detail::failure("arguments",
                               "input, format (smf or dawproject), and output are required", {}, 2);

    std::optional<pulp::timeline::Project> imported;
    std::set<std::string, std::less<>> media_paths;
    std::optional<detail::BoundedZipArchive> archive_entries;
    std::string fidelity;
    if (*format == Format::Smf) {
        auto bytes = read_file_bounded(input, pulp::timeline::SmfImportLimits{}.max_file_bytes);
        if (!bytes)
            return detail::failure("import", "could not read bounded SMF input",
                                   filesystem_path_to_utf8(input));
        auto result = pulp::timeline::import_smf(*bytes);
        if (!result)
            return detail::failure("import", result.error().message);
        fidelity = "\"exact_tick_conversion\":" +
                   std::string(result.value().exact_tick_conversion ? "true" : "false") +
                   ",\"max_tick_rounding_error\":\"" +
                   std::to_string(result.value().max_tick_rounding_error) + "\"";
        imported.emplace(std::move(result).value().project);
    } else {
        if (input.extension() != ".dawproject")
            return detail::failure("arguments", "DAWproject input must end in .dawproject",
                                   filesystem_path_to_utf8(input), 2);
        const auto limits = pulp::timeline::DawProjectImportLimits{};
        if (limits.max_media_assets > std::numeric_limits<std::size_t>::max() - 2)
            return detail::failure("import", "DAWproject media asset limit is invalid");
        const auto max_files = limits.max_media_assets + 2; // project.xml + loss manifest
        if (max_files > std::numeric_limits<std::size_t>::max() / 2)
            return detail::failure("import", "DAWproject archive entry limit is invalid");
        const auto max_entries = max_files * 2; // permit one explicit directory per file
        auto read_entries =
            detail::read_bounded_zip_archive(input, detail::kMaxAssetWorkingSetBytes, max_files,
                                             max_entries, limits.max_package_path_bytes);
        if (!read_entries)
            return detail::failure("import", read_entries.error(), filesystem_path_to_utf8(input));
        archive_entries.emplace(std::move(read_entries).value());
        const auto xml_entry = archive_entries->find("project.xml");
        if (!xml_entry || xml_entry->size() > limits.max_xml_bytes)
            return detail::failure("import",
                                   "DAWproject ZIP must contain a bounded root project.xml entry",
                                   filesystem_path_to_utf8(input));
        std::uint64_t total_media = 0;
        std::uint64_t prior_resolver_copy_charge = 0;
        auto resolver = [&](std::string_view relative) -> std::optional<std::vector<std::uint8_t>> {
            archive_entries->release_external(prior_resolver_copy_charge);
            prior_resolver_copy_charge = 0;
            const auto entry = archive_entries->find(relative);
            if (!entry || entry->size() > limits.max_media_bytes_per_resolver_call ||
                total_media > limits.max_total_media_bytes ||
                entry->size() > limits.max_total_media_bytes - total_media)
                return std::nullopt;
            const auto copy_charge = static_cast<std::uint64_t>(entry->size()) +
                                     detail::kExternalArtifactMetadataReserveBytes;
            if (!archive_entries->acquire_external(copy_charge))
                return std::nullopt;
            prior_resolver_copy_charge = copy_charge;
            total_media += entry->size();
            if (!media_paths.contains(relative)) {
                const auto path_charge = static_cast<std::uint64_t>(relative.size()) +
                                         detail::kExternalArtifactMetadataReserveBytes;
                if (!archive_entries->acquire_external(path_charge))
                    return std::nullopt;
                media_paths.insert(std::string(relative));
            }
            return std::vector<std::uint8_t>(entry->begin(), entry->end());
        };
        auto result = pulp::timeline::import_dawproject_xml(
            std::string_view(reinterpret_cast<const char*>(xml_entry->data()), xml_entry->size()),
            resolver, limits);
        if (!result)
            return detail::failure("import", result.error().message);
        archive_entries->release_external(prior_resolver_copy_charge);
        imported.emplace(std::move(result).value());
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto serialized = pulp::timeline::serialize_project(*imported, registry.value());
    if (!serialized)
        return detail::failure("import", detail::persistence_message(serialized.error()),
                               serialized.error().path);

    auto publisher = detail::AtomicPublisher::create(output_directory);
    if (!publisher)
        return detail::failure("publish",
                               "output directory must not exist and its parent must exist",
                               filesystem_path_to_utf8(output_directory));
    if (!publisher->write("project.json", serialized.value().json))
        return detail::failure("publish", "could not stage canonical project.json");
    for (const auto& path : media_paths)
        if (!publisher->write(path, *archive_entries->find(path)))
            return detail::failure("publish", "could not stage imported sibling media", path);
    if (!publisher->commit_directory())
        return detail::failure("publish", "output directory appeared before atomic publication",
                               filesystem_path_to_utf8(output_directory));
    return {0, output_json(format_text, output_directory, fidelity)};
}

} // namespace pulp::tools::timeline
