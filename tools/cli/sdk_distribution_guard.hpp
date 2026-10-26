#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::cli {

bool sdk_allows_distribution(const std::filesystem::path& build_dir, std::string& error);
// Inspects a loose dylib or a filesystem-backed artifact tree. Opaque flat
// packages, disk images, and zip archives are guarded when Pulp creates them
// from selected, inspected inputs; this helper does not extract containers.
// `follow_directory_symlinks` is off for the contents of an opened container,
// whose symlinks (a disk image's /Applications alias) point outside it.
std::filesystem::path vellum_d15_artifact_offender(const std::filesystem::path& artifact,
                                                   bool follow_directory_symlinks = true);

// PULP_DEV_SHARED_UI=ON links plug-in modules against a shared Pulp runtime
// that only exists in the build tree. False (with `error` set) when the
// configured build in `build_dir` has it on.
bool dev_shared_build_allows_distribution(const std::filesystem::path& build_dir,
                                          std::string& error);
// The first file under `artifact` (a loose file or a filesystem-backed tree)
// that is the development-only shared runtime or names it in its bytes, as a
// linked module's load command does. Returns the artifact itself when it
// cannot be inspected, and an empty path when it is clean or absent.
std::filesystem::path dev_shared_artifact_offender(const std::filesystem::path& artifact,
                                                   bool follow_directory_symlinks = true);
// Identities that sign only for loading on this machine: ad hoc ("-") and
// Apple Development certificates. A dev-shared build may be signed with these
// so a local host loads it; it may never be packaged, notarized or shared.
bool is_local_signing_identity(std::string_view identity);

// The uncompressed total `zipinfo -t` reports ("N files, X bytes uncompressed,
// ..."), or nullopt when the text does not carry one.
std::optional<std::uintmax_t> zipinfo_uncompressed_bytes(std::string_view zipinfo_totals);
// The bytes a flat package's payload expands to, from the installKBytes of
// the <payload> element in one PackageInfo document; nullopt when absent.
std::optional<std::uintmax_t> pkg_payload_install_bytes(std::string_view package_info);
// Empty when `needed` bytes fit in `available` with a fixed safety margin;
// otherwise the reason the container is not extracted.
std::string container_space_refusal(std::uintmax_t needed, std::uintmax_t available);

} // namespace pulp::cli
