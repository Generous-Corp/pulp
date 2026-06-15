// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pulp::cli::kit {

namespace fs = std::filesystem;

struct KitIssue {
    std::string severity;
    std::string code;
    std::string message;
};

struct KitSummary {
    fs::path root;
    fs::path manifest_path;
    std::string schema;
    std::string id;
    std::string name;
    std::string version;
    std::string license;
    std::vector<std::string> kinds;
    std::vector<std::string> capabilities;
    std::vector<std::string> dependency_packages;
};

struct KitValidationResult {
    KitSummary summary;
    std::vector<KitIssue> issues;

    bool ok() const;
};

KitValidationResult validate_manifest_path(const fs::path& path, bool strict = false);
std::string validation_result_json(const KitValidationResult& result);

int cmd_kit(const std::vector<std::string>& args);

}  // namespace pulp::cli::kit
