#pragma once

#include <string>
#include <vector>

namespace pulp::ship {

// Cross-platform installer creation
// macOS: uses pkgbuild/productbuild (see codesign.hpp create_pkg/create_combined_pkg)
// Windows: generates NSIS script and invokes makensis
// Linux: creates .deb or .tar.gz (see package_linux.cpp)

struct InstallerConfig {
    std::string product_name;        // e.g., "PulpGain"
    std::string publisher;           // e.g., "Generous Corp"
    std::string version;             // e.g., "1.0.0"
    std::string output_path;         // Path for the output installer file

    // Plugin bundles to install
    struct PluginBundle {
        std::string source_path;     // Path to the built plugin
        std::string install_dir;     // Target install directory
        std::string format;          // "vst3", "clap", "au", "standalone"
    };
    std::vector<PluginBundle> plugins;

    // Optional
    std::string icon_path;           // Path to .ico (Windows) or .icns (macOS)
    std::string license_path;        // Path to license text file
    std::string url;                 // Publisher URL
    bool per_user_install = false;   // Install to user directory (no admin required)
};

// Create a Windows NSIS installer (.exe)
// Requires NSIS (makensis) to be installed and on PATH
// Returns true on success
bool create_nsis_installer(const InstallerConfig& config);

// Generate the NSIS script content without invoking makensis
// Useful for inspection or manual builds
std::string generate_nsis_script(const InstallerConfig& config);

// Create Linux package artifacts from a build output directory.
// create_deb() requires dpkg-deb to be available on PATH.
bool create_deb(const std::string& plugin_name,
                const std::string& version,
                const std::string& build_dir,
                const std::string& output_path,
                const std::string& manufacturer);

bool create_tar_gz(const std::string& plugin_name,
                   const std::string& build_dir,
                   const std::string& output_path);

} // namespace pulp::ship
