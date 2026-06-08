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

}  // namespace pulp::cli::tools
