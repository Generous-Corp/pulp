#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::cli {

bool sdk_allows_distribution(const std::filesystem::path& build_dir, std::string& error);
// Inspects a loose dylib or a filesystem-backed artifact tree. Opaque flat
// packages, disk images, and zip archives are guarded when Pulp creates them
// from selected, inspected inputs; this helper does not extract containers.
std::filesystem::path vellum_d15_artifact_offender(const std::filesystem::path& artifact);

// PULP_DEV_SHARED_UI=ON links plug-in modules against a shared Pulp runtime
// that only exists in the build tree. False (with `error` set) when the
// configured build in `build_dir` has it on.
bool dev_shared_build_allows_distribution(const std::filesystem::path& build_dir,
                                          std::string& error);
// The first file under `artifact` (a loose file or a filesystem-backed tree)
// that is the development-only shared runtime or names it in its bytes, as a
// linked module's load command does. Returns the artifact itself when it
// cannot be inspected, and an empty path when it is clean or absent.
std::filesystem::path dev_shared_artifact_offender(const std::filesystem::path& artifact);
// Identities that sign only for loading on this machine: ad hoc ("-") and
// Apple Development certificates. A dev-shared build may be signed with these
// so a local host loads it; it may never be packaged, notarized or shared.
bool is_local_signing_identity(std::string_view identity);

} // namespace pulp::cli
