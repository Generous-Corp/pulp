#pragma once

#include <filesystem>
#include <string>

namespace pulp::cli {

bool sdk_allows_distribution(const std::filesystem::path& build_dir, std::string& error);
// Inspects a loose dylib or a filesystem-backed artifact tree. Opaque flat
// packages, disk images, and zip archives are guarded when Pulp creates them
// from selected, inspected inputs; this helper does not extract containers.
std::filesystem::path vellum_d15_artifact_offender(const std::filesystem::path& artifact);

} // namespace pulp::cli
