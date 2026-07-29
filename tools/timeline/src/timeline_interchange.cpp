#include <pulp/tools/timeline/agent.hpp>

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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

bool path_is_beneath(const fs::path& base, const fs::path& candidate) {
    auto left = base.begin();
    auto right = candidate.begin();
    for (; left != base.end() && right != candidate.end(); ++left, ++right)
        if (*left != *right)
            return false;
    return left == base.end() && right != candidate.end();
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

bool publish_directory_no_replace(const fs::path& source, const fs::path& destination) noexcept {
#ifdef _WIN32
    return ::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__APPLE__)
    return ::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__)
    return ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                     RENAME_NOREPLACE) == 0;
#else
    // std::filesystem has no atomic no-replace directory rename. An
    // exists-then-rename fallback races another publisher and can overwrite
    // its result, so unsupported platforms fail closed.
    (void)source;
    (void)destination;
    return false;
#endif
}

class DirectoryPublisher {
  public:
    static std::optional<DirectoryPublisher> create(const fs::path& destination) {
        if (destination.empty())
            return std::nullopt;
        std::error_code error;
        const auto status = fs::symlink_status(destination, error);
        if ((!error && status.type() != fs::file_type::not_found) ||
            (error && error != std::errc::no_such_file_or_directory))
            return std::nullopt;
        error.clear();
        fs::path parent = destination.parent_path();
        if (parent.empty())
            parent = fs::current_path(error);
        if (error || !fs::is_directory(parent, error) || error)
            return std::nullopt;

        static std::atomic<std::uint64_t> counter{0};
        const auto seed =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
            const auto suffix = seed ^ (++counter) ^ attempt;
            fs::path staging = parent / ("." + destination.filename().string() + ".pulp-staging-" +
                                         std::to_string(suffix));
            if (fs::create_directory(staging, error))
                return DirectoryPublisher(destination, std::move(staging));
            if (error != std::errc::file_exists)
                return std::nullopt;
            error.clear();
        }
        return std::nullopt;
    }

    DirectoryPublisher(DirectoryPublisher&& other) noexcept
        : destination_(std::move(other.destination_)), staging_(std::move(other.staging_)),
          committed_(other.committed_) {
        other.committed_ = true;
    }
    DirectoryPublisher& operator=(DirectoryPublisher&&) = delete;
    DirectoryPublisher(const DirectoryPublisher&) = delete;
    DirectoryPublisher& operator=(const DirectoryPublisher&) = delete;

    ~DirectoryPublisher() {
        if (!committed_) {
            std::error_code ignored;
            fs::remove_all(staging_, ignored);
        }
    }

    bool write(std::string_view relative_utf8, std::span<const std::uint8_t> bytes) {
        if (!pulp::timeline::package_relative_path_is_lexically_safe(relative_utf8))
            return false;
        fs::path relative;
        try {
            relative = filesystem_path_from_utf8(relative_utf8);
        } catch (...) {
            return false;
        }
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            relative.has_root_directory())
            return false;
        const fs::path output = staging_ / relative;
        std::error_code error;
        if (fs::exists(output, error) || error)
            return false;
        if (!fs::create_directories(output.parent_path(), error) && error)
            return false;
        try {
            std::ofstream stream(output, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;
            if (!bytes.empty())
                stream.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            return static_cast<bool>(stream);
        } catch (...) {
            return false;
        }
    }

    bool write(std::string_view relative_utf8, std::string_view text) {
        return write(relative_utf8,
                     std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    bool commit() noexcept {
        if (!publish_directory_no_replace(staging_, destination_))
            return false;
        committed_ = true;
        return true;
    }

  private:
    DirectoryPublisher(fs::path destination, fs::path staging)
        : destination_(std::move(destination)), staging_(std::move(staging)) {}

    fs::path destination_;
    fs::path staging_;
    bool committed_ = false;
};

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

std::optional<std::vector<std::uint8_t>> read_package_file(const fs::path& canonical_base,
                                                           std::string_view relative,
                                                           std::uint64_t max_bytes) {
    if (!pulp::timeline::package_relative_path_is_lexically_safe(relative))
        return std::nullopt;
    fs::path path;
    try {
        path = filesystem_path_from_utf8(relative);
    } catch (...) {
        return std::nullopt;
    }
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return std::nullopt;
    std::error_code error;
    const auto candidate = fs::canonical(canonical_base / path, error);
    if (error || !path_is_beneath(canonical_base, candidate) ||
        !fs::is_regular_file(candidate, error) || error)
        return std::nullopt;
    return read_file_bounded(candidate, max_bytes);
}

bool add_dawproject_media_impl(const detail::LoadedProject& loaded,
                               pulp::interchange::ExportArtifacts& artifacts,
                               std::uint64_t max_total_media_bytes) {
    const auto* root = loaded.value.find_sequence(loaded.value.root_sequence_id());
    if (root == nullptr)
        return false;
    std::set<std::string> artifact_names;
    for (const auto& artifact : artifacts.artifacts)
        artifact_names.insert(artifact.name);
    std::set<std::uint64_t> seen_assets;
    std::uint64_t retained_media_bytes = 0;
    std::vector<pulp::interchange::ExportArtifact> media_artifacts;
    for (const auto& track : root->tracks()) {
        for (const auto& clip : track.clips()) {
            if (clip.time_anchor() != pulp::timeline::ClipTimeAnchor::Musical)
                continue;
            const auto* media = std::get_if<pulp::timeline::MediaRef>(&clip.content());
            if (media == nullptr || !seen_assets.insert(media->asset_id.value).second)
                continue;
            const auto* asset = loaded.value.find_asset(media->asset_id);
            if (asset == nullptr || asset->name.empty())
                return false;
            if (!pulp::timeline::package_relative_path_is_lexically_safe(asset->name))
                return false;
            const std::string artifact_name = "audio/" + asset->name;
            if (!artifact_names.insert(artifact_name).second)
                return false;
            if (retained_media_bytes > max_total_media_bytes)
                return false;
            auto bytes = detail::read_verified_asset_bytes(
                loaded, *asset, max_total_media_bytes - retained_media_bytes);
            if (!bytes)
                return false;
            retained_media_bytes += static_cast<std::uint64_t>(bytes->size());
            media_artifacts.push_back({artifact_name, std::move(*bytes)});
        }
    }
    artifacts.artifacts.insert(artifacts.artifacts.end(),
                               std::make_move_iterator(media_artifacts.begin()),
                               std::make_move_iterator(media_artifacts.end()));
    return true;
}

} // namespace

namespace detail {

bool add_dawproject_media(const LoadedProject& loaded,
                          pulp::interchange::ExportArtifacts& artifacts,
                          std::uint64_t max_total_media_bytes) {
    return add_dawproject_media_impl(loaded, artifacts, max_total_media_bytes);
}

} // namespace detail

OperationResult export_project(const ProjectSource& project, std::string_view format_text,
                               const fs::path& output_directory,
                               const std::vector<std::string>& accepted_loss_names) {
    const auto format = parse_format(format_text);
    if (!format || output_directory.empty())
        return detail::failure("arguments", "format (smf or dawproject) and output are required",
                               {}, 2);

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
    if (!exported)
        return detail::failure(
            "export", exported.error().message,
            exported.error().concepts.empty()
                ? std::string_view{}
                : pulp::interchange::concept_id(exported.error().concepts.front()));
    if (*format == Format::DawProject &&
        !detail::add_dawproject_media(loaded.value(), exported.value()))
        return detail::failure("export", "could not materialize referenced DAWproject media");

    auto publisher = DirectoryPublisher::create(output_directory);
    if (!publisher)
        return detail::failure("publish",
                               "output directory must not exist and its parent must exist",
                               filesystem_path_to_utf8(output_directory));
    for (const auto& artifact : exported.value().artifacts)
        if (!publisher->write(artifact.name, artifact.bytes))
            return detail::failure("publish", "could not stage export artifact", artifact.name);
    if (!publisher->commit())
        return detail::failure("publish", "output directory appeared before atomic publication",
                               filesystem_path_to_utf8(output_directory));
    return {0, output_json(format_text, output_directory,
                           std::string("\"lossless\":") + (plan.is_lossless() ? "true" : "false"))};
}

OperationResult import_project(const fs::path& input, std::string_view format_text,
                               const fs::path& output_directory) {
    const auto format = parse_format(format_text);
    if (!format || input.empty() || output_directory.empty())
        return detail::failure("arguments",
                               "input, format (smf or dawproject), and output are required", {}, 2);

    std::optional<pulp::timeline::Project> imported;
    std::map<std::string, std::vector<std::uint8_t>> media;
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
        if (input.filename() != "project.xml")
            return detail::failure("arguments",
                                   "DAWproject import requires an unpacked project.xml input",
                                   filesystem_path_to_utf8(input), 2);
        const auto limits = pulp::timeline::DawProjectImportLimits{};
        auto xml = read_file_bounded(input, limits.max_xml_bytes);
        if (!xml)
            return detail::failure("import", "could not read bounded project.xml input",
                                   filesystem_path_to_utf8(input));
        std::error_code error;
        const auto base = fs::canonical(input.parent_path(), error);
        if (error)
            return detail::failure("import", "could not resolve the DAWproject input directory");
        std::uint64_t total_media = 0;
        auto resolver = [&](std::string_view relative) -> std::optional<std::vector<std::uint8_t>> {
            auto bytes =
                read_package_file(base, relative, limits.max_media_bytes_per_resolver_call);
            if (!bytes || total_media > limits.max_total_media_bytes ||
                bytes->size() > limits.max_total_media_bytes - total_media)
                return std::nullopt;
            total_media += bytes->size();
            media[std::string(relative)] = *bytes;
            return bytes;
        };
        auto result = pulp::timeline::import_dawproject_xml(
            std::string_view(reinterpret_cast<const char*>(xml->data()), xml->size()), resolver,
            limits);
        if (!result)
            return detail::failure("import", result.error().message);
        imported.emplace(std::move(result).value());
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto serialized = pulp::timeline::serialize_project(*imported, registry.value());
    if (!serialized)
        return detail::failure("import", detail::persistence_message(serialized.error()),
                               serialized.error().path);

    auto publisher = DirectoryPublisher::create(output_directory);
    if (!publisher)
        return detail::failure("publish",
                               "output directory must not exist and its parent must exist",
                               filesystem_path_to_utf8(output_directory));
    if (!publisher->write("project.json", serialized.value().json))
        return detail::failure("publish", "could not stage canonical project.json");
    for (const auto& [path, bytes] : media)
        if (!publisher->write(path, bytes))
            return detail::failure("publish", "could not stage imported sibling media", path);
    if (!publisher->commit())
        return detail::failure("publish", "output directory appeared before atomic publication",
                               filesystem_path_to_utf8(output_directory));
    return {0, output_json(format_text, output_directory, fidelity)};
}

} // namespace pulp::tools::timeline
