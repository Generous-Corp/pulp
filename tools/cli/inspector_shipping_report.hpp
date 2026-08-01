#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace pulp::cli::inspector_shipping {
namespace fs = std::filesystem;

struct Report {
    std::string json = "[]";
    bool ships_inspector = false;
    bool ships_runtime_eval = false;
};

inline Report load_report(const fs::path& build_dir) {
    Report report;
    auto dir = build_dir / "pulp-inspector-manifests";
    if (!fs::is_directory(dir)) dir = build_dir;
    if (!fs::is_directory(dir)) return report;
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        if (dir == build_dir &&
            entry.path().filename().string().find(".inspector-capabilities.json") ==
                std::string::npos)
            continue;
        std::ifstream input(entry.path());
        std::string manifest((std::istreambuf_iterator<char>(input)), {});
        if (manifest.empty()) continue;
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
