#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli::inspector_shipping {
namespace fs = std::filesystem;

struct Manifest {
    fs::path path;
    std::string json;
    std::string target;
    std::string product_name;
    std::string control_profile;
    std::string build_id;
    std::string registry_digest;
    std::string artifact_digest;
    std::string control_manifest_digest;
    std::string consent_identity;
    std::vector<std::string> capabilities;
    std::vector<std::string> control_capabilities;
    bool ships_inspector = false;
    bool ships_runtime_eval = false;
    bool remote_view_authority = false;
    bool osc_udp_network_surface = false;
};

struct Report {
    std::string json = "[]";
    std::vector<Manifest> manifests;
    bool ships_inspector = false;
    bool ships_runtime_eval = false;
    bool complete = false;
    std::string error;
    std::string error_code;
};

Report empty_report();
Report load_report(
    const std::vector<fs::path>& search_roots,
    std::optional<std::string_view> product = std::nullopt);
Report load_report(
    const fs::path& search_root,
    std::optional<std::string_view> product = std::nullopt);
Report load_artifact_report(const fs::path& search_root);
Report audit_artifact(const fs::path& artifact_or_bundle);
Report load_exact_artifact_report(
    const fs::path& artifact, const fs::path& manifest_root,
    std::optional<std::string_view> product = std::nullopt);
Report combine_reports(std::vector<Report> reports);
bool scan_artifact(const fs::path& artifact, const Manifest& manifest,
                   std::string& error);
bool write_evidence(const fs::path& path, const Report& report,
                    std::string_view operation);
std::string audit_json(const Report& report);
std::string audit_human(const Report& report);

} // namespace pulp::cli::inspector_shipping
