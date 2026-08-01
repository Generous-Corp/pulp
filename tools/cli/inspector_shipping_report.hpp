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
    std::string product_name;
    std::vector<std::string> capabilities;
    bool ships_inspector = false;
    bool ships_runtime_eval = false;
};

struct Report {
    std::string json = "[]";
    std::vector<Manifest> manifests;
    bool ships_inspector = false;
    bool ships_runtime_eval = false;
    bool complete = false;
    std::string error;
};

Report empty_report();
Report load_report(
    const std::vector<fs::path>& search_roots,
    std::optional<std::string_view> product = std::nullopt);
Report load_report(
    const fs::path& search_root,
    std::optional<std::string_view> product = std::nullopt);
Report load_artifact_report(const fs::path& search_root);
Report combine_reports(std::vector<Report> reports);
bool scan_artifact(const fs::path& artifact, const Manifest& manifest,
                   std::string& error);
bool write_evidence(const fs::path& path, const Report& report,
                    std::string_view operation);

} // namespace pulp::cli::inspector_shipping
