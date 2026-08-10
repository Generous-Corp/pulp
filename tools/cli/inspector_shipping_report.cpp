#include "inspector_shipping_report.hpp"

#include <pulp/inspect/control_manifest.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace pulp::cli::inspector_shipping {
namespace {

bool scan_artifact_bytes(const fs::path& artifact, std::string_view binary,
                         const Manifest& manifest, std::string& error);

bool known_capability(std::string_view candidate) {
#define PULP_INSPECT_CAPABILITY( \
    symbol, legacy_id, contract_id, risk, side_effect, executor, evidence, \
    observe, develop, grantable, publication_bound) \
    if (candidate == legacy_id) return true;
#include <pulp/inspect/capability_definitions.inc>
#undef PULP_INSPECT_CAPABILITY
    return false;
}

const pulp::inspect::InspectorCapabilityDescriptor* control_descriptor(
    std::string_view contract_id) {
    auto capability = pulp::inspect::capability_from_contract_id(contract_id);
    if (!capability)
        capability = pulp::inspect::capability_from_id(contract_id);
    if (!capability) return nullptr;
    for (const auto& descriptor :
         pulp::inspect::inspector_capability_registry()) {
        if (descriptor.capability == *capability) return &descriptor;
    }
    return nullptr;
}

bool read_manifest(const fs::path& path, Manifest& manifest, std::string& error,
                   std::string* error_code = nullptr) {
    std::error_code status_error;
    const auto link_status = fs::symlink_status(path, status_error);
    if (!status_error && fs::is_symlink(link_status)) {
        if (error_code) *error_code = "audit.symlink-forbidden";
        error = "control audit does not follow inspector capability manifest "
                "symlinks: " + path.string();
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        error = "could not read inspector capability manifest: " + path.string();
        return false;
    }
    manifest.path = path;
    constexpr std::size_t kMaximumManifestBytes = 1024 * 1024;
    manifest.json.resize(kMaximumManifestBytes + 1);
    input.read(manifest.json.data(),
               static_cast<std::streamsize>(manifest.json.size()));
    const auto bytes_read = static_cast<std::size_t>(input.gcount());
    if (bytes_read > kMaximumManifestBytes) {
        error = "inspector capability manifest exceeds 1 MiB: " + path.string();
        return false;
    }
    manifest.json.resize(bytes_read);
    if (input.bad() || manifest.json.empty()) {
        error = "could not read inspector capability manifest: " + path.string();
        return false;
    }

    try {
        const auto root = choc::json::parse(manifest.json);
        if (root.isObject() && root.hasObjectMember("schema")) {
            pulp::inspect::ControlManifestDiagnostics diagnostics;
            const auto control =
                pulp::inspect::parse_control_manifest(manifest.json, &diagnostics);
            if (!control) {
                const auto stable_code = std::string(
                    pulp::inspect::control_manifest_error_id(diagnostics.code));
                if (error_code) *error_code = stable_code;
                error = "invalid control manifest [" + stable_code + "]: " +
                    path.string() + ": " + diagnostics.error;
                return false;
            }
            manifest.target = control->target;
            manifest.product_name = control->product_name;
            manifest.control_profile = std::string(
                pulp::inspect::control_profile_id(control->profile));
            manifest.build_id = control->build_id;
            manifest.registry_digest = control->registry_digest;
            manifest.control_manifest_digest =
                pulp::inspect::control_manifest_digest(*control);
            manifest.ships_inspector = control->endpoint_included;
            manifest.ships_runtime_eval =
                control->unsafe_runtime_eval_acknowledged;
            for (const auto capability : control->capabilities) {
                manifest.capabilities.emplace_back(
                    pulp::inspect::capability_id(capability));
                manifest.control_capabilities.emplace_back(
                    pulp::inspect::capability_contract_id(capability));
            }
            return true;
        }
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

const std::string& standalone_artifact_marker() {
    // Build the sentinel at runtime so the scanner executable does not carry
    // the exact byte sequence it is looking for and identify itself as a
    // product artifact when validating a build tree.
    static const std::string marker = [] {
        std::string value = "PULP_STANDALONE_";
        value += "COMPONENT_V1";
        return value;
    }();
    return marker;
}

bool read_artifact_bytes(const fs::path& artifact, std::string& binary,
                         std::string& error) {
    std::ifstream input(artifact, std::ios::binary);
    if (!input) {
        error = "could not read artifact for control audit: " + artifact.string();
        return false;
    }
    binary.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad() || binary.empty()) {
        error = "could not read artifact for control audit: " + artifact.string();
        return false;
    }
    return true;
}

struct ArtifactSnapshot {
    std::string bytes;
    std::string error;
    bool readable = false;
};

class ArtifactSnapshotCache {
public:
    const ArtifactSnapshot& get(const fs::path& artifact) {
        const auto key = artifact.lexically_normal();
        auto [entry, inserted] = snapshots_.try_emplace(key);
        if (inserted) {
            entry->second.readable = read_artifact_bytes(
                artifact, entry->second.bytes, entry->second.error);
        }
        return entry->second;
    }

private:
    std::map<fs::path, ArtifactSnapshot> snapshots_;
};

bool artifact_has_standalone_marker(const fs::path& artifact,
                                    ArtifactSnapshotCache& snapshots) {
    const auto& snapshot = snapshots.get(artifact);
    return snapshot.readable &&
        snapshot.bytes.find(standalone_artifact_marker()) != std::string::npos;
}

void detect_artifact_surfaces(std::string_view binary, Manifest& manifest) {
    manifest.artifact_digest = pulp::runtime::sha256_hex(binary);
    manifest.consent_identity = pulp::inspect::control_consent_identity(
        manifest.control_manifest_digest, manifest.artifact_digest);
    manifest.remote_view_authority =
        binary.find("PULP_REMOTE_VIEW_PARAMETER_AUTHORITY_V1") !=
            std::string::npos ||
        binary.find("view.param_set") != std::string::npos;
    manifest.osc_udp_network_surface =
        binary.find("PULP_OSC_UDP_NETWORK_SURFACE_V1") != std::string::npos;
}

bool scan_and_detect_artifact(const fs::path& artifact, Manifest& manifest,
                              std::string& error,
                              ArtifactSnapshotCache* snapshots = nullptr) {
    std::string owned_binary;
    std::string_view binary;
    if (snapshots) {
        const auto& snapshot = snapshots->get(artifact);
        if (!snapshot.readable) {
            error = snapshot.error;
            return false;
        }
        binary = snapshot.bytes;
    } else {
        if (!read_artifact_bytes(artifact, owned_binary, error)) return false;
        binary = owned_binary;
    }
    detect_artifact_surfaces(binary, manifest);
    return scan_artifact_bytes(artifact, binary, manifest, error);
}

bool is_runnable_candidate(const fs::path& artifact) {
    std::error_code error;
    const auto link_status = fs::symlink_status(artifact, error);
    if (error || fs::is_symlink(link_status) ||
        !fs::is_regular_file(link_status))
        return false;
    const auto extension = artifact.extension().string();
    if (extension == ".a" || extension == ".lib" || extension == ".o" ||
        extension == ".obj")
        return false;
    const auto permissions = fs::status(artifact, error).permissions();
    if (error) return false;
    return extension == ".exe" ||
        (permissions & (fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec)) != fs::perms::none;
}

bool has_matching_inspector_sidecar(const fs::path& executable,
                                    ArtifactSnapshotCache& snapshots) {
    const auto directory = executable.parent_path();
    const auto executable_name = executable.filename().string();
    std::error_code error;
    std::size_t sidecar_count = 0;
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto path = iterator->path();
        const auto filename = path.filename().string();
        if (iterator->is_symlink() || !iterator->is_regular_file() ||
            !filename.ends_with(".inspector-capabilities.json"))
            continue;
        ++sidecar_count;
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
    if (error || sidecar_count != 1 ||
        !artifact_has_standalone_marker(executable, snapshots))
        return false;
    std::size_t standalone_count = 0;
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (is_runnable_candidate(iterator->path()) &&
            artifact_has_standalone_marker(iterator->path(), snapshots))
            ++standalone_count;
    }
    return !error && standalone_count == 1;
}

bool safe_artifact_component(std::string_view value) {
    if (value.empty() || value == "." || value == ".." ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos)
        return false;
    const fs::path component{std::string(value)};
    return !component.is_absolute() && component.filename() == component;
}

bool inside_plugin_format(const fs::path& path) {
    for (const auto& component : path) {
        auto extension = component.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (extension == ".vst3" || extension == ".clap" ||
            extension == ".component" || extension == ".appex" ||
            extension == ".aaxplugin" || extension == ".lv2")
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
        if (!read_manifest(manifest_path, manifest, report.error,
                           &report.error_code))
            return report;
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
    ArtifactSnapshotCache artifact_snapshots;

    // Refuse linked evidence before resolving any executable. Directory
    // iteration order must not let a real artifact report a linked sidecar as
    // merely missing before the linked entry itself is visited.
    std::error_code symlink_error;
    for (fs::recursive_directory_iterator iterator(
             search_root, fs::directory_options::skip_permission_denied,
             symlink_error), end;
         !symlink_error && iterator != end; iterator.increment(symlink_error)) {
        if (inside_plugin_format(iterator->path())) {
            if (iterator->is_directory()) iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->path().filename().string().ends_with(
                ".inspector-capabilities.json") &&
            iterator->is_symlink()) {
            Report failed;
            failed.error_code = "audit.symlink-forbidden";
            failed.error =
                "control audit does not follow inspector capability manifest "
                "symlinks: " + iterator->path().string();
            return failed;
        }
    }
    if (symlink_error) {
        Report failed;
        failed.error_code = "audit.manifest-enumeration";
        failed.error = "could not enumerate inspector capability manifests in: " +
            search_root.string();
        return failed;
    }

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
        if (inside_plugin_format(path)) {
            if (artifact_iterator->is_directory())
                artifact_iterator.disable_recursion_pending();
            artifact_iterator.increment(artifact_error);
            continue;
        }
        if (path.filename().string().ends_with(
                ".inspector-capabilities.json") &&
            artifact_iterator->is_symlink()) {
            Report failed;
            failed.error_code = "audit.symlink-forbidden";
            failed.error =
                "control audit does not follow inspector capability manifest "
                "symlinks: " + path.string();
            return failed;
        }
        if (artifact_iterator->is_regular_file()) {
            const bool executable = is_runnable_candidate(path);
            const bool configured_standalone = has_configured_manifests &&
                matches_configured_standalone(path, configured);
            if (executable &&
                !has_matching_inspector_sidecar(path, artifact_snapshots) &&
                (configured_standalone ||
                 artifact_has_standalone_marker(path, artifact_snapshots))) {
                Report missing;
                missing.error =
                    "standalone artifact is missing inspector capability sidecar: " +
                    path.string();
                return missing;
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
        if (inside_plugin_format(path)) {
            if (iterator->is_directory()) iterator.disable_recursion_pending();
            iterator.increment(iteration_error);
            continue;
        }
        const auto filename = path.filename().string();
        if (filename.ends_with(".inspector-capabilities.json")) {
            Manifest manifest;
            Report report;
            if (iterator->is_symlink()) {
                report.error_code = "audit.symlink-forbidden";
                report.error =
                    "control audit does not follow inspector capability manifest "
                    "symlinks: " + path.string();
                return report;
            }
            if (!iterator->is_regular_file()) {
                iterator.increment(iteration_error);
                continue;
            }
            if (!read_manifest(path, manifest, report.error,
                               &report.error_code))
                return report;

            const auto suffix = std::string(".inspector-capabilities.json");
            const auto target = filename.substr(0, filename.size() - suffix.size());
            if (!safe_artifact_component(manifest.product_name) ||
                (!manifest.target.empty() &&
                 !safe_artifact_component(manifest.target)) ||
                !safe_artifact_component(target)) {
                report.error_code = "audit.manifest-invalid";
                report.error =
                    "control manifest artifact identity must be a safe filename beside its sidecar: " +
                    path.string();
                return report;
            }
            const auto canonical = !manifest.control_profile.empty();
            if (canonical && manifest.target != target) {
                report.error_code = "audit.manifest-invalid";
                report.error =
                    "canonical control manifest target must match its sidecar filename: " +
                    path.string();
                return report;
            }
            std::vector<fs::path> exact_candidates = {
                path.parent_path() / manifest.product_name,
                path.parent_path() / (manifest.product_name + ".exe"),
            };
            if (!manifest.target.empty()) {
                exact_candidates.push_back(path.parent_path() / manifest.target);
                exact_candidates.push_back(
                    path.parent_path() / (manifest.target + ".exe"));
            }
            if (!canonical) {
                exact_candidates.push_back(path.parent_path() / target);
                exact_candidates.push_back(
                    path.parent_path() / (target + ".exe"));
            }
            std::sort(exact_candidates.begin(), exact_candidates.end());
            exact_candidates.erase(
                std::unique(exact_candidates.begin(), exact_candidates.end()),
                exact_candidates.end());
            for (const auto& candidate : exact_candidates) {
                std::error_code candidate_error;
                if (fs::is_symlink(fs::symlink_status(candidate,
                                                       candidate_error))) {
                    report.error_code = "audit.symlink-forbidden";
                    report.error =
                        "control audit does not follow artifact symlinks: " +
                        candidate.string();
                    return report;
                }
            }
            std::vector<fs::path> existing;
            std::copy_if(exact_candidates.begin(), exact_candidates.end(),
                         std::back_inserter(existing),
                         is_runnable_candidate);
            if (existing.empty() && !canonical) {
                std::error_code sibling_error;
                for (fs::directory_iterator sibling(path.parent_path(), sibling_error),
                     sibling_end;
                     !sibling_error && sibling != sibling_end;
                     sibling.increment(sibling_error)) {
                    if (is_runnable_candidate(sibling->path()) &&
                        artifact_has_standalone_marker(sibling->path(),
                                                       artifact_snapshots))
                        existing.push_back(sibling->path());
                }
                if (sibling_error) {
                    report.error =
                        "could not enumerate standalone artifacts beside inspector capability sidecar: " +
                        path.string();
                    return report;
                }
                std::sort(existing.begin(), existing.end());
                existing.erase(std::unique(existing.begin(), existing.end()),
                               existing.end());
            }
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
            if (!scan_and_detect_artifact(existing.front(), manifest,
                                          report.error, &artifact_snapshots))
                return report;
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

Report audit_artifact(const fs::path& artifact_or_bundle) {
    std::error_code status_error;
    const auto link_status = fs::symlink_status(artifact_or_bundle, status_error);
    if (!status_error && fs::is_symlink(link_status)) {
        Report report;
        report.error_code = "audit.symlink-forbidden";
        report.error = "control audit does not follow artifact symlinks: " +
            artifact_or_bundle.string();
        return report;
    }
    status_error.clear();
    const auto absolute_target =
        fs::absolute(artifact_or_bundle, status_error).lexically_normal();
    if (status_error) {
        Report report;
        report.error_code = "audit.target-missing";
        report.error = "could not resolve control audit target: " +
            artifact_or_bundle.string();
        return report;
    }
    status_error.clear();
    const auto resolved_target = fs::weakly_canonical(absolute_target, status_error);
    if (status_error) {
        Report report;
        report.error_code = "audit.target-missing";
        report.error = "could not resolve control audit target: " +
            artifact_or_bundle.string();
        return report;
    }
    if (inside_plugin_format(absolute_target) ||
        inside_plugin_format(resolved_target)) {
        Report report;
        report.error_code = "audit.no-artifact";
        report.error =
            "control audit found no auditable Pulp standalone artifact; "
            "plugin-format paths are excluded: " +
            artifact_or_bundle.string();
        return report;
    }
    // All subsequent reads use the resolved path. A caller-controlled parent
    // symlink therefore cannot retarget sidecar selection after classification
    // or hide a plugin-format component behind an alias.
    const auto& audit_target = resolved_target;
    status_error.clear();
    if (fs::is_directory(audit_target, status_error) && !status_error) {
        auto report = load_artifact_report(audit_target);
        if (report.complete && report.manifests.empty()) {
            report.complete = false;
            report.error_code = "audit.no-artifact";
            report.error =
                "control audit found no auditable Pulp standalone artifact "
                "and manifest in: " + audit_target.string();
        }
        if (report.complete && std::any_of(
                report.manifests.begin(), report.manifests.end(),
                [](const Manifest& manifest) {
                    return manifest.control_profile.empty();
                })) {
            report.complete = false;
            report.error_code = "audit.canonical-manifest-required";
            report.error =
                "control audit requires a canonical artifact manifest; legacy "
                "shipping evidence is report-only";
        }
        if (!report.complete && report.error_code.empty())
            report.error_code = "audit.artifact-evidence-invalid";
        return report;
    }
    if (!fs::is_regular_file(audit_target, status_error) || status_error) {
        Report report;
        report.error_code = "audit.target-missing";
        report.error = "control audit target does not exist: " +
            artifact_or_bundle.string();
        return report;
    }

    const auto parent = audit_target.parent_path().empty()
        ? fs::current_path()
        : audit_target.parent_path();
    const auto filename = audit_target.filename().string();
    const auto exact_sidecar =
        parent / (filename + ".inspector-capabilities.json");
    std::vector<fs::path> candidates;
    if (fs::is_regular_file(exact_sidecar)) {
        candidates.push_back(exact_sidecar);
    } else {
        std::error_code iteration_error;
        for (fs::directory_iterator iterator(parent, iteration_error), end;
             !iteration_error && iterator != end;
             iterator.increment(iteration_error)) {
            const auto candidate = iterator->path();
            if (iterator->is_regular_file() &&
                candidate.filename().string().ends_with(
                    ".inspector-capabilities.json"))
                candidates.push_back(candidate);
        }
        if (iteration_error) {
            Report report;
            report.error_code = "audit.manifest-enumeration";
            report.error = "could not enumerate control manifests beside: " +
                                  audit_target.string();
            return report;
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    std::vector<Manifest> matches;
    std::string parse_error;
    std::string parse_error_code;
    for (const auto& candidate : candidates) {
        if (!fs::is_regular_file(candidate)) continue;
        Manifest manifest;
        if (!read_manifest(candidate, manifest, parse_error, &parse_error_code)) {
            Report report;
            report.error = std::move(parse_error);
            report.error_code = parse_error_code.empty()
                ? "audit.manifest-invalid"
                : std::move(parse_error_code);
            return report;
        }
        if (!safe_artifact_component(manifest.product_name) ||
            (!manifest.target.empty() &&
             !safe_artifact_component(manifest.target))) {
            Report report;
            report.error_code = "audit.manifest-invalid";
            report.error =
                "control manifest artifact identity must be a safe filename beside its sidecar: " +
                candidate.string();
            return report;
        }
        const auto suffix = std::string(".inspector-capabilities.json");
        const auto sidecar_name = candidate.filename().string();
        const auto sidecar_stem =
            sidecar_name.substr(0, sidecar_name.size() - suffix.size());
        const auto canonical = !manifest.control_profile.empty();
        if (canonical && manifest.target != sidecar_stem) {
            Report report;
            report.error_code = "audit.manifest-invalid";
            report.error =
                "canonical control manifest target must match its sidecar filename: " +
                candidate.string();
            return report;
        }
        const auto identity_matches =
            manifest.target == filename ||
            (!manifest.target.empty() &&
             manifest.target + ".exe" == filename) ||
            manifest.product_name == filename ||
            manifest.product_name + ".exe" == filename;
        if (candidate == exact_sidecar && !identity_matches) {
            Report report;
            report.error_code = "audit.manifest-invalid";
            report.error =
                "exact control manifest artifact identity does not match artifact filename: " +
                candidate.string();
            return report;
        }
        if (identity_matches)
            matches.push_back(std::move(manifest));
    }
    if (matches.size() != 1) {
        Report report;
        report.error_code = matches.empty() ? "audit.manifest-missing"
                                            : "audit.manifest-ambiguous";
        report.error = matches.empty()
            ? "control manifest is missing beside artifact: " +
                  audit_target.string()
            : "multiple control manifests match artifact: " +
                  artifact_or_bundle.string();
        return report;
    }

    Report report;
    report.manifests.push_back(std::move(matches.front()));
    if (report.manifests.front().control_profile.empty()) {
        report.error_code = "audit.canonical-manifest-required";
        report.error =
            "control audit requires a canonical artifact manifest; legacy "
            "shipping evidence is report-only";
        return report;
    }
    if (!scan_and_detect_artifact(audit_target,
                                  report.manifests.front(), report.error)) {
        report.error_code = "audit.artifact-verification";
        return report;
    }
    report.complete = true;
    report.ships_inspector = report.manifests.front().ships_inspector;
    report.ships_runtime_eval = report.manifests.front().ships_runtime_eval;
    report.json = "[\n" + report.manifests.front().json + "\n]";
    return report;
}

Report load_exact_artifact_report(
    const fs::path& artifact, const fs::path& manifest_root,
    std::optional<std::string_view> product) {
    ArtifactSnapshotCache artifact_snapshots;
    if (!artifact_has_standalone_marker(artifact, artifact_snapshots))
        return empty_report();
    auto report = load_report(manifest_root, product);
    if (!report.complete) return report;
    if (report.manifests.size() != 1) {
        report.complete = false;
        report.error = "expected exactly one inspector capability manifest for " +
            artifact.string();
        return report;
    }
    if (!scan_and_detect_artifact(artifact, report.manifests.front(),
                                  report.error, &artifact_snapshots))
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

namespace {
bool scan_artifact_bytes(const fs::path& artifact, std::string_view binary,
                         const Manifest& manifest, std::string& error) {
    if (!manifest.control_profile.empty()) {
        pulp::inspect::ControlArtifactExpectation expectation;
        expectation.profile_id = manifest.control_profile;
        expectation.manifest_digest = manifest.control_manifest_digest;
        expectation.endpoint_included = manifest.ships_inspector;
        expectation.runtime_eval_included = manifest.ships_runtime_eval;
        expectation.capability_ids = manifest.capabilities;
        const auto validation =
            pulp::inspect::validate_control_artifact_bytes(binary, expectation);
        if (!validation.valid)
            error = validation.error + ": " + artifact.string();
        return validation.valid;
    }
    const auto contains = [&](std::string_view token) {
        return binary.find(token) != std::string::npos;
    };
    if (!contains(standalone_artifact_marker())) {
        error = "inspector capability manifest is not paired with a Pulp standalone artifact: " +
            artifact.string();
        return false;
    }
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
} // namespace

bool scan_artifact(const fs::path& artifact, const Manifest& manifest,
                   std::string& error) {
    std::string binary;
    return read_artifact_bytes(artifact, binary, error) &&
           scan_artifact_bytes(artifact, binary, manifest, error);
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

std::string audit_json(const Report& report) {
    auto root = choc::value::createObject("");
    root.addMember("schema", choc::value::createString("pulp.control.audit.v1"));
    root.addMember("ok", choc::value::createBool(report.complete));
    root.addMember("verdict", choc::value::createString(
        report.complete ? "pass" : "block"));
    if (!report.error.empty())
        root.addMember("error", choc::value::createString(report.error));
    if (!report.error_code.empty())
        root.addMember("errorCode", choc::value::createString(report.error_code));
    auto manifests = choc::value::createEmptyArray();
    for (const auto& manifest : report.manifests) {
        auto item = choc::value::createObject("");
        item.addMember("target", choc::value::createString(manifest.target));
        item.addMember("product", choc::value::createString(manifest.product_name));
        item.addMember("profile", choc::value::createString(
            manifest.control_profile.empty() ? "legacy" : manifest.control_profile));
        if (!manifest.build_id.empty())
            item.addMember("buildId",
                           choc::value::createString(manifest.build_id));
        if (!manifest.registry_digest.empty())
            item.addMember("registryDigest",
                           choc::value::createString(manifest.registry_digest));
        if (!manifest.artifact_digest.empty())
            item.addMember("artifactDigest",
                           choc::value::createString(manifest.artifact_digest));
        if (!manifest.consent_identity.empty())
            item.addMember("consentIdentity",
                           choc::value::createString(manifest.consent_identity));
        item.addMember("endpointIncluded",
                       choc::value::createBool(manifest.ships_inspector));
        item.addMember("runtimeEvalIncluded",
                       choc::value::createBool(manifest.ships_runtime_eval));
        auto surfaces = choc::value::createEmptyArray();
        if (manifest.remote_view_authority)
            surfaces.addArrayElement(choc::value::createString(
                "remote-view-parameter-authority"));
        if (manifest.osc_udp_network_surface)
            surfaces.addArrayElement(choc::value::createString(
                "osc-udp-network"));
        item.addMember("detectedSurfaces", surfaces);
        auto capabilities = choc::value::createEmptyArray();
        const auto& ids = manifest.control_capabilities.empty()
            ? manifest.capabilities
            : manifest.control_capabilities;
        for (const auto& capability : ids)
            capabilities.addArrayElement(choc::value::createString(capability));
        item.addMember("capabilities", capabilities);
        auto details = choc::value::createEmptyArray();
        for (const auto& capability : ids) {
            const auto* descriptor = control_descriptor(capability);
            if (!descriptor) continue;
            auto detail = choc::value::createObject("");
            detail.addMember("id", choc::value::createString(capability));
            detail.addMember("risk", choc::value::createString(
                pulp::inspect::capability_risk_id(descriptor->risk)));
            detail.addMember("sideEffect", choc::value::createString(
                pulp::inspect::side_effect_id(descriptor->side_effect)));
            detail.addMember("executor", choc::value::createString(
                pulp::inspect::executor_id(descriptor->executor)));
            detail.addMember("evidence", choc::value::createString(
                pulp::inspect::evidence_id(descriptor->evidence)));
            details.addArrayElement(detail);
        }
        item.addMember("capabilityDetails", details);
        if (!manifest.control_manifest_digest.empty())
            item.addMember("manifestDigest", choc::value::createString(
                manifest.control_manifest_digest));
        manifests.addArrayElement(item);
    }
    root.addMember("artifacts", manifests);
    auto advisories = choc::value::createEmptyArray();
    if (report.complete && report.manifests.size() == 1 &&
        report.manifests.front().control_profile.empty())
        advisories.addArrayElement(choc::value::createString(
            "legacy manifest: migrate to CONTROL_PROFILE and CONTROL_CAPABILITIES"));
    for (const auto& manifest : report.manifests) {
        bool contains_mutation = false;
        bool contains_critical = false;
        const auto& ids = manifest.control_capabilities.empty()
            ? manifest.capabilities
            : manifest.control_capabilities;
        for (const auto& capability : ids) {
            const auto* descriptor = control_descriptor(capability);
            if (!descriptor) continue;
            contains_mutation = contains_mutation ||
                descriptor->risk ==
                    pulp::inspect::InspectorCapabilityRisk::Control ||
                descriptor->risk ==
                    pulp::inspect::InspectorCapabilityRisk::HighRisk;
            contains_critical = contains_critical ||
                descriptor->risk ==
                    pulp::inspect::InspectorCapabilityRisk::Critical;
        }
        if (contains_mutation)
            advisories.addArrayElement(choc::value::createString(
                "mutating capabilities require an explicit client grant and controller lease at runtime"));
        if (contains_critical)
            advisories.addArrayElement(choc::value::createString(
                "critical capability present: review the research-unsafe profile and acknowledgement"));
        if (manifest.osc_udp_network_surface)
            advisories.addArrayElement(choc::value::createString(
                "OSC UDP is a separate network surface outside Product A policy"));
    }
    root.addMember("advisories", advisories);
    return choc::json::toString(root, false);
}

std::string audit_human(const Report& report) {
    std::ostringstream out;
    out << (report.complete ? "PASS" : "BLOCK") << " control artifact audit\n";
    if (!report.error.empty()) out << "  " << report.error << "\n";
    for (const auto& manifest : report.manifests) {
        out << "  product: " << manifest.product_name << "\n"
            << "  profile: "
            << (manifest.control_profile.empty() ? "legacy"
                                                  : manifest.control_profile)
            << "\n"
            << (manifest.build_id.empty() ? "" : "  build ID: " + manifest.build_id + "\n")
            << (manifest.registry_digest.empty()
                    ? ""
                    : "  registry digest: " + manifest.registry_digest + "\n")
            << (manifest.artifact_digest.empty()
                    ? ""
                    : "  artifact digest: " + manifest.artifact_digest + "\n")
            << (manifest.consent_identity.empty()
                    ? ""
                    : "  consent identity: " + manifest.consent_identity + "\n")
            << "  endpoint: "
            << (manifest.ships_inspector ? "included" : "absent") << "\n"
            << "  capabilities:";
        const auto& ids = manifest.control_capabilities.empty()
            ? manifest.capabilities
            : manifest.control_capabilities;
        if (ids.empty()) out << " none";
        for (const auto& capability : ids) {
            out << "\n    - " << capability;
            if (const auto* descriptor = control_descriptor(capability)) {
                out << " [risk="
                    << pulp::inspect::capability_risk_id(descriptor->risk)
                    << ", side-effect="
                    << pulp::inspect::side_effect_id(descriptor->side_effect)
                    << ", executor="
                    << pulp::inspect::executor_id(descriptor->executor)
                    << ", evidence="
                    << pulp::inspect::evidence_id(descriptor->evidence) << "]";
            }
        }
        out << "\n";
        if (manifest.control_profile.empty())
            out << "  advisory: migrate to CONTROL_PROFILE and "
                   "CONTROL_CAPABILITIES\n";
        if (manifest.remote_view_authority)
            out << "  detected: Remote View parameter authority\n";
        if (manifest.osc_udp_network_surface)
            out << "  detected: OSC UDP network surface (outside Product A)\n";
        bool mutation_advisory = false;
        bool critical_advisory = false;
        for (const auto& capability : ids) {
            const auto* descriptor = control_descriptor(capability);
            if (!descriptor) continue;
            mutation_advisory = mutation_advisory ||
                descriptor->risk ==
                    pulp::inspect::InspectorCapabilityRisk::Control ||
                descriptor->risk ==
                    pulp::inspect::InspectorCapabilityRisk::HighRisk;
            critical_advisory = critical_advisory ||
                descriptor->risk ==
                    pulp::inspect::InspectorCapabilityRisk::Critical;
        }
        if (mutation_advisory)
            out << "  advisory: runtime mutation still requires a client grant "
                   "and controller lease\n";
        if (critical_advisory)
            out << "  advisory: review the research-unsafe profile and exact "
                   "runtime-eval acknowledgement\n";
    }
    return out.str();
}

} // namespace pulp::cli::inspector_shipping
