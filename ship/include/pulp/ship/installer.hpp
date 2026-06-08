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

// Debian architecture string matching the architecture this code was
// compiled for. The plugins packaged on a native build share the ship
// library's target arch, so this is the correct `Architecture:` field for
// the generated `.deb`. Header-only so it is available (and testable) on
// every platform without depending on the Linux-only package TU.
inline std::string debian_architecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__arm__)
    return "armhf";
#elif defined(__i386__)
    return "i386";
#else
    return "amd64";  // best-effort default for unrecognised hosts
#endif
}

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

// Wrap a standalone executable into a single-file AppImage. `exe_path` is the
// built standalone binary; `icon_path` is an optional .png (a placeholder is
// used when empty). Requires `appimagetool` on PATH — returns false honestly
// when it (or the executable) is absent. Linux-only; defined in
// ship/platform/linux/package_linux.cpp.
bool create_appimage(const std::string& app_name,
                     const std::string& version,
                     const std::string& exe_path,
                     const std::string& output_path,
                     const std::string& icon_path = "");

} // namespace pulp::ship
