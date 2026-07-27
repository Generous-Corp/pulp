#pragma once

#include <filesystem>
#include <string>

namespace pulp::cli {

bool sdk_allows_distribution(const std::filesystem::path& build_dir, std::string& error);

} // namespace pulp::cli
