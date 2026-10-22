#pragma once

#include <filesystem>
#include <string>

namespace pulp::cli {

// `use_ninja` adds `-G Ninja`. Pass it only for a build dir with no
// CMakeCache.txt yet: CMake refuses to change an existing dir's generator.
std::string create_standalone_configure_command(const std::filesystem::path& source_dir,
                                                const std::filesystem::path& build_dir,
                                                bool debug_build,
                                                const std::filesystem::path& sdk_dir,
                                                bool use_ninja);

std::string create_standalone_ctest_command(const std::filesystem::path& build_dir,
                                            bool debug_build);

}  // namespace pulp::cli
