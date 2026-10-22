// Ship-time local-symbol stripping for macOS plug-in bundles.
//
// A plug-in's first load after install costs time in proportion to the size of
// its module, and local symbols are a large share of a Release module that no
// host, crash reporter or dyld needs. `pulp ship package` therefore ships a
// copy of each bundle whose main executable has had `strip -x` applied, and
// re-signs that copy the way the original was signed. The bundle in the build
// directory keeps its symbols, so debugging and the nm-based packaging checks
// keep working against it.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace pulp::cli {

struct ShipStripOptions {
    // Developer ID Application identity for a copy of a bundle that was signed
    // with one. Empty means a Developer-ID-signed bundle ships unstripped.
    std::string identity;
    std::string entitlements;
    // Runs the unattended signing preflight before the first identity signing;
    // returns false to refuse.
    std::function<bool()> signing_preflight;
};

// True when `file` starts with a Mach-O or universal-binary magic number.
bool is_macho_file(const std::filesystem::path& file);

// The bundle's CFBundleExecutable, falling back to the bundle's stem.
std::string bundle_executable_name(const std::filesystem::path& bundle);

// Copies `bundle` under `staging_root`, strips local symbols from the copy's
// main executable and re-signs the copy: with `options.identity` when the
// original carries a Developer ID signature, ad hoc otherwise. Returns the path
// to package: the copy, or `bundle` itself when there is nothing to strip (no
// Mach-O main executable) or no identity to re-sign a Developer ID bundle with.
// Returns nullopt, after reporting why, when a step fails.
std::optional<std::filesystem::path>
stage_stripped_bundle(const std::filesystem::path& bundle,
                      const std::filesystem::path& staging_root, const ShipStripOptions& options);

} // namespace pulp::cli
