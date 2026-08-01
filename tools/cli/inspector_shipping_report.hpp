#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli::inspector_shipping {
namespace fs = std::filesystem;

struct Report {
    std::string json = "[]";
    bool ships_inspector = false;
    bool ships_runtime_eval = false;
    bool complete = false;
    std::string error;
};

inline Report load_report(
    const fs::path& build_dir,
    const std::optional<std::string_view> product = std::nullopt) {
    Report report;
    auto dir = build_dir / "pulp-inspector-manifests";
    if (!fs::is_directory(dir)) dir = build_dir;
    if (!fs::is_directory(dir)) {
        report.error = "inspector capability manifest directory is missing: " +
            build_dir.string();
        return report;
    }
    std::vector<fs::path> manifests;
    std::error_code iteration_error;
    fs::directory_iterator iterator(dir, iteration_error);
    fs::directory_iterator end;
    while (!iteration_error && iterator != end) {
        const auto& entry = *iterator;
        if (entry.is_regular_file() && entry.path().extension() == ".json" &&
            (dir != build_dir ||
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
    std::sort(manifests.begin(), manifests.end());

    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& manifest_path : manifests) {
        std::ifstream input(manifest_path);
        if (!input) {
            report.error = "could not read inspector capability manifest: " +
                manifest_path.string();
            return report;
        }
        std::string manifest((std::istreambuf_iterator<char>(input)), {});
        if (input.bad() || manifest.empty()) {
            report.error = "could not read inspector capability manifest: " +
                manifest_path.string();
            return report;
        }
        if (manifest.find("\"shipping_override\": ") == std::string::npos ||
            manifest.find("\"unsafe_runtime_eval_acknowledged\": ") == std::string::npos ||
            manifest.find("\"capabilities\":") == std::string::npos) {
            report.error = "invalid inspector capability manifest: " +
                manifest_path.string();
            return report;
        }
        if (product) {
            const auto product_field =
                "\"product_name\": \"" + std::string(*product) + "\"";
            if (manifest.find(product_field) == std::string::npos) continue;
        }
        report.ships_inspector |=
            manifest.find("\"shipping_override\": true") != std::string::npos;
        report.ships_runtime_eval |=
            manifest.find("\"unsafe_runtime_eval_acknowledged\": true") != std::string::npos;
        if (!first) json << ",";
        json << "\n" << manifest;
        first = false;
    }
    if (!first) json << "\n";
    json << "]";
    report.json = json.str();
    if (first) {
        report.error = product
            ? "no inspector capability manifest matches product '" +
                std::string(*product) + "'"
            : "no inspector capability manifests found in " + dir.string();
        return report;
    }
    report.complete = true;
    return report;
}

inline bool write_evidence(const fs::path& path, const Report& report,
                           std::string_view operation) {
    std::ofstream output(path);
    if (!output) return false;
    output << "{\n  \"schema_version\": 1,\n  \"operation\": \""
           << operation << "\",\n  \"inspector_capabilities\": "
           << report.json << "\n}\n";
    return output.good();
}
} // namespace pulp::cli::inspector_shipping
