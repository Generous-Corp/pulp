// SPDX-License-Identifier: MIT
#pragma once

#include "package_registry.hpp"

#include <pulp/audio/analyzer_provider.hpp>

#include <span>
#include <vector>

namespace pulp::cli::pkg {

[[nodiscard]] std::vector<pulp::audio::AnalyzerCapability>
package_analyzer_capabilities(const PackageDescriptor& package);

[[nodiscard]] bool package_has_analyzer_capabilities(const PackageDescriptor& package);

// Converts package-registry metadata into core/audio analyzer descriptors for
// UI/control-thread discovery. This does not install packages, fetch network
// state, or enter realtime paths. All supplied targets must be supported; pass
// only the active/current target for per-platform discovery.
[[nodiscard]] std::vector<pulp::audio::AnalyzerDescriptor>
package_analyzer_descriptors(const PackageDescriptor& package,
                             const LockFile& lock_file,
                             std::span<const PlatformTarget> targets,
                             bool allow_restricted_licenses = false);

[[nodiscard]] std::vector<pulp::audio::AnalyzerDescriptor>
registry_analyzer_descriptors(const Registry& registry,
                              const LockFile& lock_file,
                              std::span<const PlatformTarget> targets,
                              bool allow_restricted_licenses = false);

}  // namespace pulp::cli::pkg
