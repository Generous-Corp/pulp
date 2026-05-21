#pragma once

#include <string>
#include <vector>

namespace pulp::cli {

std::vector<std::string> create_default_build_targets(const std::string& class_name,
                                                      const std::string& type,
                                                      const std::string& formats);

// Returns the CMake build configuration name ("Release" by default, "Debug"
// when debug is true).
//
// Multi-config generators (Visual Studio, Xcode) ignore the configure-time
// CMAKE_BUILD_TYPE and require the configuration to be chosen at BUILD time via
// `cmake --build --config <cfg>` and at TEST time via `ctest -C <cfg>`.
// Single-config generators (Ninja, Unix Makefiles) honor CMAKE_BUILD_TYPE and
// safely ignore these flags, so emitting `--config`/`-C` unconditionally is
// correct on every generator. Codex P1 on PR #2133: setting only the
// configure-time CMAKE_BUILD_TYPE left `pulp create` building Debug on Visual
// Studio (and made `--debug` a no-op there).
std::string create_build_config(bool debug);

}  // namespace pulp::cli
