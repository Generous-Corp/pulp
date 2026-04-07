// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pulp::cli::pkg {

namespace fs = std::filesystem;

int cmd_add(const std::vector<std::string>& args);
int cmd_remove(const std::vector<std::string>& args);
int cmd_list(const std::vector<std::string>& args);
int cmd_search(const std::vector<std::string>& args);
int cmd_update(const std::vector<std::string>& args);
int cmd_suggest(const std::vector<std::string>& args);
int cmd_target(const std::vector<std::string>& args);

int audit_packages(const fs::path& project_root);
int audit_platforms(const fs::path& project_root);
int audit_licenses(const fs::path& project_root);

}  // namespace pulp::cli::pkg
