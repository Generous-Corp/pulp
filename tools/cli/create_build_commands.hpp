#pragma once

#include <filesystem>
#include <string>

namespace pulp::cli {

std::string create_standalone_configure_command(const std::filesystem::path& source_dir,
                                                const std::filesystem::path& build_dir,
                                                bool debug_build,
                                                const std::filesystem::path& sdk_dir);

std::string create_standalone_ctest_command(const std::filesystem::path& build_dir,
                                            bool debug_build);

}  // namespace pulp::cli
