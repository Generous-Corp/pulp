// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp::cli::tools {

namespace fs = std::filesystem;

// ── Tool Descriptor ──

struct BinarySource {
    std::string url_template;    // with ${version} placeholder
    std::string archive_format;  // "tar.gz", "zip", "tar.xz"
    std::string binary_name;     // e.g., "uv", "ffmpeg.exe"
};

// Per-platform packaged-artifact source for an importer add-on. Unlike
// BinarySource (a single extractable binary), an importer ships an archive
// holding the extractor substrate + thin shim + a bundled libclang, so we
// verify the whole archive by sha256 before unpacking the install tree.
struct ImporterArtifact {
    std::string url_template;    // with ${version} placeholder; supports file:// + bare paths
    std::string archive_format;  // "tar.gz", "zip", "tar.xz"
    std::string sha256;          // expected hex digest of the artifact, verified pre-install
};

struct ToolDescriptor {
    std::string id;
    std::string display_name;
    std::string category;        // "runtime", "binary", "python_tool"
    std::string description;
    std::string license;
    std::string install_method;  // "binary_download", "python_pip"
    std::map<std::string, BinarySource> binary_sources;
    std::string pip_package;     // for python_pip
    std::string pinned_version;
    std::vector<std::string> requires_tools;
    bool managed_by_pulp = true;
    bool bundleable = false;

    // ── Project-importer fields (optional) ──
    //
    // Present only on framework-importer tools (vendor-specific add-ons that
    // drive the JSON-over-stdio import SPI). All runtime DATA — the registry
    // names frameworks/vendors, the SDK code does not. `spi_min`/`spi_max`
    // bound the SPI versions the importer speaks; a mismatch fails loudly.
    // `sdk_min`/`sdk_max` bound the Pulp SDK versions it targets.
    std::vector<std::string> frameworks;     // framework ids this tool imports
    int spi_min = 0;                         // 0 when unset (no importer fields)
    int spi_max = 0;
    std::string sdk_min;                     // semver string, empty when unset
    std::string sdk_max;
    std::vector<std::string> capabilities;   // e.g. "detect", "analyze", "emit"
    std::string health_check;                // command string to probe the tool

    // ── IMPORTER_TERMS fields (optional, DATA) ──
    //
    // The accept-to-run terms body an importer presents before it runs, plus
    // its version and an opaque vendor id for the audit trail. The terms text
    // is vendor-supplied DATA; the SDK only surfaces it, hashes it, and records
    // acceptance. Absent on tools that declare no terms (the gate passes).
    // The install path (#19) records `terms_version` + `vendor_id` into the
    // importer install record so the terms-gate composes with packaging.
    std::string terms_text;
    std::string terms_version;
    std::string vendor_id;

    // ── Importer packaging fields (optional, #19) ──
    //
    // Present on importer add-ons distributed as checksummed per-platform
    // archives. `importer_artifacts` is keyed by platform key (e.g.
    // "macOS-arm64"); each artifact carries its own sha256 so install verifies
    // the fetched/local package before unpacking. `skill_source` is the path of
    // the per-importer SKILL.md inside the archive; `skill_name` is the skills
    // directory to install it under (defaults to `id`).
    std::map<std::string, ImporterArtifact> importer_artifacts;
    std::string skill_source;                // relative SKILL.md path inside the archive
    std::string skill_name;                  // skills dir name (defaults to id)
};

// ── Tool Registry ──

struct ToolRegistry {
    int schema_version = 0;
    std::map<std::string, ToolDescriptor> tools;
};

struct ToolRegistryLoadResult {
    ToolRegistry registry;
    std::string error;
};

ToolRegistryLoadResult load_tool_registry(const fs::path& path);

// ── Tool Status ──

enum class ToolStatus {
    not_installed,
    installed,
    outdated,
    missing_dependency,
    unavailable,
};

struct ToolLocateResult {
    bool found = false;
    fs::path path;
    std::string source;  // "pulp-managed", "system-path", "not-found"
    std::string version;
};

struct ToolInstallResult {
    bool ok = false;
    fs::path binary_path;
    std::string installed_version;
    std::string error;
};

// ── Pulp Home ──

fs::path pulp_home();
fs::path tools_dir();

// ── Current Platform ──

std::string current_platform_key();

// ── Tool Operations ──

ToolLocateResult locate_tool(const ToolDescriptor& tool);
ToolInstallResult install_binary_tool(const ToolDescriptor& tool, bool force = false);
ToolInstallResult install_python_tool(const ToolDescriptor& tool,
                                       const ToolRegistry& registry,
                                       bool force = false);
bool uninstall_tool(const std::string& tool_id);

// ── Archive Extraction ──

bool extract_archive(const fs::path& archive, const fs::path& dest,
                     const std::string& format);

// ── CLI Command ──

int cmd_tool(const std::vector<std::string>& args);

// ── Importer Install (#19) ──
//
// Skills directory honoring $PULP_HOME (~/.agents/skills/<importer>/...).
// Records of installed importers live under `pulp_home()/importers/`.
fs::path skills_dir();
fs::path importer_records_dir();

// Parse a "MAJOR.MINOR.PATCH" prefix into a comparable tuple. Trailing
// pre-release / build metadata (after the first non-numeric run) is ignored.
// Returns false when the leading component is not a number.
struct Semver3 {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool operator<(const Semver3& o) const;
    bool operator<=(const Semver3& o) const;
};
bool parse_semver3(const std::string& s, Semver3& out);

// Result of validating an importer against the running SDK + SPI window.
struct ImporterCompatResult {
    bool ok = false;
    std::string error;  // loud, user-facing version-mismatch message when !ok
};

// Validate that `sdk_version` is within [sdk_min, sdk_max] (inclusive, when
// bounds are set) and that the importer's [spi_min, spi_max] window overlaps
// the SDK's supported SPI window [sdk_spi_min, sdk_spi_max].
ImporterCompatResult check_importer_compat(const ToolDescriptor& tool,
                                           const std::string& sdk_version,
                                           int sdk_spi_min,
                                           int sdk_spi_max);

// Compute the SHA-256 hex digest of a file. Empty string on read failure.
std::string sha256_file_hex(const fs::path& path);

struct ImporterInstallResult {
    bool ok = false;
    std::string installed_version;
    fs::path install_dir;     // unpacked importer tree under pulp_home()/tools/<id>/<ver>
    fs::path skill_path;      // installed SKILL.md path (empty when none shipped)
    fs::path record_path;     // install record JSON under importer_records_dir()
    std::string error;
};

// Install an importer add-on from its registry descriptor.
//   sdk_version    — running SDK version (CLI passes PULP_SDK_VERSION)
//   sdk_spi_min/max— the SPI window this Pulp build speaks
//   from_override  — when non-empty, install from this local path / file:// URL
//                    instead of the registry url_template (test + offline path)
//   force          — reinstall even if an up-to-date record exists
ImporterInstallResult install_importer(const ToolDescriptor& tool,
                                       const std::string& sdk_version,
                                       int sdk_spi_min,
                                       int sdk_spi_max,
                                       const std::string& from_override = {},
                                       bool force = false);

// Remove an installed importer: deletes the install tree, its skill directory,
// and the install record. Returns false when nothing was installed.
bool uninstall_importer(const std::string& importer_id);

// `pulp tool install/uninstall <importer>` dispatch + the `pulp add` alias.
// Returns std::nullopt when `id` is not an importer (caller falls back to the
// generic binary/python tool path).
std::optional<int> handle_importer_install(const ToolRegistry& registry,
                                            const std::string& id,
                                            const std::string& from_override,
                                            bool force);
std::optional<int> handle_importer_uninstall(const ToolRegistry& registry,
                                              const std::string& id);

// Locate the repo's tools/packages/tool-registry.json by walking up from cwd.
// Empty path when not found.
fs::path find_tool_registry_path();

// `pulp add <importer>` alias entry point: if `id` names an importer in the
// tool-registry, route to the importer install path and return its exit code;
// otherwise return std::nullopt so `pulp add` continues with normal package
// resolution.
std::optional<int> try_add_importer_alias(const std::string& id,
                                          const std::string& from_override,
                                          bool force);

}  // namespace pulp::cli::tools
