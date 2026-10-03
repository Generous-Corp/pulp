// SPDX-License-Identifier: MIT
#pragma once

#include "kit_commands.hpp"
#include "kit_json_helpers.hpp"

#include <string>
#include <utility>
#include <vector>

namespace pulp::cli::kit {

struct KitProfileResult {
    std::string path;
    std::string kind;
    std::string status;
    std::vector<std::pair<std::string, std::string>> artifacts;
    std::vector<KitIssue> issues;
};

struct KitVerifyOptions {
    bool execute_screenshots = false;
    fs::path screenshot_output_dir;
    std::string screenshot_backend = "auto";
};

std::string profile_results_json(const std::vector<KitProfileResult>& profiles);
bool profile_ok(const KitProfileResult& profile);
std::vector<KitProfileResult> verify_profiles(const fs::path& manifest_path,
                                              const JsonValue& manifest,
                                              const fs::path& project_root,
                                              const KitVerifyOptions& options);

} // namespace pulp::cli::kit
