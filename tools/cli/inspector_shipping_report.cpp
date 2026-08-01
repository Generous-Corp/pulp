#include "inspector_shipping_report.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>

namespace pulp::cli::inspector_shipping {
namespace {

bool known_capability(std::string_view candidate) {
#define PULP_INSPECT_CAPABILITY(symbol, id, risk, observe, develop, grantable, publication_bound) \
    if (candidate == id) return true;
#include <pulp/inspect/capability_definitions.inc>
#undef PULP_INSPECT_CAPABILITY
    return false;
}

bool read_manifest(const fs::path& path, Manifest& manifest, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "could not read inspector capability manifest: " + path.string();
        return false;
    }
    manifest.path = path;
    manifest.json.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad() || manifest.json.empty()) {
        error = "could not read inspector capability manifest: " + path.string();
        return false;
    }

    try {
        const auto root = choc::json::parse(manifest.json);
        if (!root.isObject() || !root["product_name"].isString() ||
            !root["shipping_override"].isBool() ||
            !root["unsafe_runtime_eval_acknowledged"].isBool() ||
            !root["capabilities"].isArray()) {
            error = "invalid inspector capability manifest: " + path.string();
            return false;
        }
        manifest.product_name = std::string(root["product_name"].getString());
        if (root["target"].isString())
            manifest.target = std::string(root["target"].getString());
        manifest.ships_inspector = root["shipping_override"].getBool();
        manifest.ships_runtime_eval =
            root["unsafe_runtime_eval_acknowledged"].getBool();
        const auto capabilities = root["capabilities"];
        for (std::size_t i = 0; i < capabilities.size(); ++i) {
            const auto capability = capabilities[static_cast<std::uint32_t>(i)];
            if (!capability.isString()) {
                error = "invalid inspector capability manifest: " + path.string();
                return false;
            }
            const std::string_view capability_id = capability.getString();
            if (!known_capability(capability_id)) {
                error = "unknown inspector capability '" +
                    std::string(capability_id) + "' in manifest: " + path.string();
                return false;
            }
            manifest.capabilities.emplace_back(capability_id);
        }
    } catch (const std::exception&) {
        error = "invalid inspector capability manifest: " + path.string();
        return false;
    }
    return true;
}

std::string capability_marker(std::string_view capability) {
    std::string marker = "PULP_INSPECT_CAPABILITY_";
    for (const auto character : capability) {
        marker.push_back(std::isalnum(static_cast<unsigned char>(character))
            ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
            : '_');
    }
    marker += "_V1";
    return marker;
}

bool artifact_has_capability_marker(const fs::path& artifact) {
    std::ifstream input(artifact, std::ios::binary);
    if (!input) return false;
    const std::string binary(std::istreambuf_iterator<char>(input), {});
#define PULP_INSPECT_CAPABILITY(symbol, id, risk, observe, develop, grantable, publication_bound) \
    if (binary.find(capability_marker(id)) != std::string::npos) return true;
#include <pulp/inspect/capability_definitions.inc>
#undef PULP_INSPECT_CAPABILITY
    return false;
}

bool has_matching_inspector_sidecar(const fs::path& executable) {
    const auto directory = executable.parent_path();
    const auto executable_name = executable.filename().string();
    std::error_code error;
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto path = iterator->path();
        const auto filename = path.filename().string();
        if (!iterator->is_regular_file() ||
            !filename.ends_with(".inspector-capabilities.json"))
            continue;
        constexpr std::string_view suffix = ".inspector-capabilities.json";
        const auto sidecar_target =
            filename.substr(0, filename.size() - suffix.size());
        if (sidecar_target == executable_name ||
            sidecar_target + ".exe" == executable_name)
            return true;
        Manifest manifest;
        std::string ignored_error;
        if (!read_manifest(path, manifest, ignored_error)) continue;
        if (manifest.product_name == executable_name ||
            manifest.product_name + ".exe" == executable_name ||
            manifest.target == executable_name ||
            manifest.target + ".exe" == executable_name)
            return true;
    }
    return false;
}

bool inside_plugin_format(const fs::path& path) {
    for (const auto& component : path) {
        const auto directory = component.string();
        if (directory == "VST3" || directory == "CLAP" || directory == "AU" ||
            directory == "AUv3" || directory == "AAX" || directory == "LV2")
            return true;
    }
    return false;
}

bool matches_configured_standalone(const fs::path& executable,
                                   const Report& configured) {
    const auto name = executable.filename().string();
    return std::any_of(configured.manifests.begin(), configured.manifests.end(),
        [&](const Manifest& manifest) {
            return name == manifest.product_name ||
                name == manifest.product_name + ".exe" ||
                (!manifest.target.empty() &&
                 (name == manifest.target || name == manifest.target + ".exe"));
        });
}

} // namespace

Report empty_report() {
    Report report;
    report.complete = true;
    return report;
}

Report load_report(const std::vector<fs::path>& search_roots,
                   std::optional<std::string_view> product) {
    Report report;
    std::vector<fs::path> manifests;
    for (const auto& search_root : search_roots) {
        auto dir = search_root / "pulp-inspector-manifests";
        if (!fs::is_directory(dir)) dir = search_root;
        if (!fs::is_directory(dir)) {
            report.error = "inspector capability manifest directory is missing: " +
                search_root.string();
            return report;
        }
        std::error_code iteration_error;
        fs::directory_iterator iterator(dir, iteration_error);
        fs::directory_iterator end;
        while (!iteration_error && iterator != end) {
            const auto& entry = *iterator;
            if (entry.is_regular_file() && entry.path().extension() == ".json" &&
                (dir != search_root ||
                 entry.path().filename().string().find(".inspector-capabilities.json") !=
                     std::string::npos)) {
                manifests.push_back(entry.path());
            }
            iterator.increment(iteration_error);
        }
        if (iteration_error) {
            report.error = "could not enumerate inspector capability manifests in " +
                dir.string();
            return report;
        }
    }
    std::sort(manifests.begin(), manifests.end());
    manifests.erase(std::unique(manifests.begin(), manifests.end()), manifests.end());

    std::ostringstream json;
    json << "[";
    for (const auto& manifest_path : manifests) {
        Manifest manifest;
        if (!read_manifest(manifest_path, manifest, report.error)) return report;
        if (product && manifest.product_name != *product) continue;
        report.ships_inspector |= manifest.ships_inspector;
        report.ships_runtime_eval |= manifest.ships_runtime_eval;
        if (!report.manifests.empty()) json << ",";
        json << "\n" << manifest.json;
        report.manifests.push_back(std::move(manifest));
    }
    if (!report.manifests.empty()) json << "\n";
    json << "]";
    report.json = json.str();
    if (report.manifests.empty()) {
        report.error = product
            ? "no inspector capability manifest matches product '" +
                std::string(*product) + "'"
            : "no inspector capability manifests found";
        return report;
    }
    report.complete = true;
    return report;
}

Report load_report(const fs::path& search_root,
                   std::optional<std::string_view> product) {
    return load_report(std::vector<fs::path>{search_root}, product);
}

Report load_artifact_report(const fs::path& search_root) {
    std::vector<Report> reports;
    if (!fs::is_directory(search_root)) return empty_report();

    const auto configured_manifest_dir =
        search_root / "pulp-inspector-manifests";
    bool has_configured_manifests = false;
    if (fs::is_directory(configured_manifest_dir)) {
        std::error_code configured_error;
        for (fs::directory_iterator iterator(configured_manifest_dir,
                                              configured_error), end;
             !configured_error && iterator != end; iterator.increment(configured_error)) {
            if (iterator->is_regular_file() &&
                iterator->path().extension() == ".json") {
                has_configured_manifests = true;
                break;
            }
        }
        if (configured_error) {
            Report failed;
            failed.error = "could not enumerate configured inspector manifests in " +
                configured_manifest_dir.string();
            return failed;
        }
    }
    Report configured;
    if (has_configured_manifests) {
        configured = load_report(search_root);
        if (!configured.complete) return configured;
    }

    // Search every standalone artifact, even in a mixed tree with configured
    // manifests. Removed or renamed targets must not disappear merely because
    // another target still has configure-time evidence.
    std::error_code artifact_error;
    fs::recursive_directory_iterator artifact_iterator(
        search_root, fs::directory_options::skip_permission_denied,
        artifact_error);
    fs::recursive_directory_iterator artifact_end;
    while (!artifact_error && artifact_iterator != artifact_end) {
        const auto path = artifact_iterator->path();
        if (artifact_iterator->is_directory() && path.extension() == ".app" &&
            !inside_plugin_format(path)) {
            const auto executable = path / "Contents/MacOS" / path.stem();
            if (fs::is_regular_file(executable) &&
                !has_matching_inspector_sidecar(executable) &&
                ((has_configured_manifests &&
                  matches_configured_standalone(executable, configured)) ||
                 artifact_has_capability_marker(executable))) {
                Report missing;
                missing.error =
                    "standalone artifact is missing inspector capability sidecar: " +
                    executable.string();
                return missing;
            }
        } else if (artifact_iterator->is_regular_file()) {
            if (!inside_plugin_format(path)) {
                const auto permissions = artifact_iterator->status().permissions();
                const bool executable = path.extension() == ".exe" ||
                    (permissions & (fs::perms::owner_exec | fs::perms::group_exec |
                                    fs::perms::others_exec)) != fs::perms::none;
                const bool configured_standalone = has_configured_manifests &&
                    matches_configured_standalone(path, configured);
                if (executable && !has_matching_inspector_sidecar(path) &&
                    (configured_standalone || artifact_has_capability_marker(path))) {
                    Report missing;
                    missing.error =
                        "standalone artifact is missing inspector capability sidecar: " +
                        path.string();
                    return missing;
                }
            }
        }
        artifact_iterator.increment(artifact_error);
    }
    if (artifact_error) {
        Report failed;
        failed.error = "could not enumerate standalone artifacts in " +
            search_root.string();
        return failed;
    }

    std::error_code iteration_error;
    fs::recursive_directory_iterator iterator(
        search_root, fs::directory_options::skip_permission_denied, iteration_error);
    fs::recursive_directory_iterator end;
    std::set<fs::path> resolved_executables;
    while (!iteration_error && iterator != end) {
        const auto path = iterator->path();
        const auto filename = path.filename().string();
        if (iterator->is_regular_file() &&
            filename.ends_with(".inspector-capabilities.json")) {
            Manifest manifest;
            Report report;
            if (!read_manifest(path, manifest, report.error)) return report;

            const auto suffix = std::string(".inspector-capabilities.json");
            const auto target = filename.substr(0, filename.size() - suffix.size());
            std::vector<fs::path> candidates = {
                path.parent_path() / manifest.product_name,
                path.parent_path() / (manifest.product_name + ".exe"),
                path.parent_path() / target,
                path.parent_path() / (target + ".exe"),
            };
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
            std::vector<fs::path> existing;
            std::copy_if(candidates.begin(), candidates.end(),
                         std::back_inserter(existing),
                         [](const fs::path& candidate) {
                             return fs::is_regular_file(candidate);
                         });
            if (existing.empty()) {
                report.error =
                    "could not resolve standalone executable for inspector capability "
                    "scan beside: " + path.string();
                return report;
            }
            if (existing.size() != 1) {
                report.error =
                    "inspector capability sidecar resolves to multiple standalone "
                    "executables: " + path.string();
                return report;
            }
            if (!resolved_executables.insert(existing.front()).second) {
                report.error =
                    "multiple inspector capability sidecars resolve to standalone "
                    "executable: " + existing.front().string();
                return report;
            }
            if (!scan_artifact(existing.front(), manifest, report.error)) return report;
            report.complete = true;
            report.ships_inspector = manifest.ships_inspector;
            report.ships_runtime_eval = manifest.ships_runtime_eval;
            report.json = "[\n" + manifest.json + "\n]";
            report.manifests.push_back(std::move(manifest));
            reports.push_back(std::move(report));
        }
        iterator.increment(iteration_error);
    }
    if (iteration_error) {
        Report report;
        report.error = "could not enumerate inspector artifact evidence in " +
            search_root.string();
        return report;
    }
    return combine_reports(std::move(reports));
}

Report load_exact_artifact_report(
    const fs::path& artifact, const fs::path& manifest_root,
    std::optional<std::string_view> product) {
    auto report = load_report(manifest_root, product);
    if (!report.complete) return report;
    if (report.manifests.size() != 1) {
        report.complete = false;
        report.error = "expected exactly one inspector capability manifest for " +
            artifact.string();
        return report;
    }
    if (!scan_artifact(artifact, report.manifests.front(), report.error))
        report.complete = false;
    return report;
}

Report combine_reports(std::vector<Report> reports) {
    Report combined = empty_report();
    std::ostringstream json;
    json << "[";
    for (auto& report : reports) {
        if (!report.complete) return report;
        for (auto& manifest : report.manifests) {
            if (!combined.manifests.empty()) json << ",";
            json << "\n" << manifest.json;
            combined.ships_inspector |= manifest.ships_inspector;
            combined.ships_runtime_eval |= manifest.ships_runtime_eval;
            combined.manifests.push_back(std::move(manifest));
        }
    }
    if (!combined.manifests.empty()) json << "\n";
    json << "]";
    combined.json = json.str();
    return combined;
}

bool scan_artifact(const fs::path& artifact, const Manifest& manifest,
                   std::string& error) {
    std::ifstream input(artifact, std::ios::binary);
    if (!input) {
        error = "could not read artifact for inspector capability scan: " +
            artifact.string();
        return false;
    }
    const std::string binary(std::istreambuf_iterator<char>(input), {});
    if (input.bad() || binary.empty()) {
        error = "could not read artifact for inspector capability scan: " +
            artifact.string();
        return false;
    }
    const auto contains = [&](std::string_view token) {
        return binary.find(token) != std::string::npos;
    };
    const bool has_shipping_marker = contains("PULP_INSPECT_SHIPPING_MANIFEST_V1");
    if (has_shipping_marker != manifest.ships_inspector) {
        error = "artifact inspector endpoint marker does not match manifest: " +
            artifact.string();
        return false;
    }

    std::vector<std::string> declared_markers;
    declared_markers.reserve(manifest.capabilities.size());
    for (const auto& capability : manifest.capabilities) {
        declared_markers.push_back(capability_marker(capability));
        if (!contains(declared_markers.back())) {
            error = "artifact is missing declared inspector capability marker '" +
                capability + "': " + artifact.string();
            return false;
        }
    }
    constexpr std::string_view capability_prefix = "PULP_INSPECT_CAPABILITY_";
    std::size_t marker_position = 0;
    while ((marker_position = binary.find(capability_prefix, marker_position)) !=
           std::string::npos) {
        const auto marker_end = binary.find("_V1", marker_position);
        if (marker_end == std::string::npos) break;
        const auto marker = binary.substr(marker_position, marker_end + 3 - marker_position);
        if (std::find(declared_markers.begin(), declared_markers.end(), marker) ==
            declared_markers.end()) {
            error = "artifact contains an undeclared inspector capability marker: " +
                artifact.string();
            return false;
        }
        marker_position = marker_end + 3;
    }

    constexpr std::string_view runtime_eval_marker =
        "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1";
    if (contains(runtime_eval_marker) != manifest.ships_runtime_eval) {
        error = "artifact runtime.eval component marker does not match manifest: " +
            artifact.string();
        return false;
    }
    return true;
}

bool write_evidence(const fs::path& path, const Report& report,
                    std::string_view operation) {
    std::ofstream output(path);
    if (!output) return false;
    output << "{\n  \"schema_version\": 1,\n  \"operation\": \""
           << operation << "\",\n  \"inspector_capabilities\": "
           << report.json << "\n}\n";
    return output.good();
}

} // namespace pulp::cli::inspector_shipping
