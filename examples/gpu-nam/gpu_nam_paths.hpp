#pragma once

// Path / resource resolution for GPU NAM: where the default model, the installed
// sample content (models + cabinet IRs), and the vendored UI assets live at
// runtime. These are IO/packaging concerns shared by the processor (default model
// + assets) and the editor's file choosers (sample-content directory), so they
// live here rather than inside the DSP processor header.
//
// All resolution follows the same order: an explicit environment override, then
// the copy bundled next to the running binary (plugin Resources / executable
// dir), then the source-tree path baked in at build time. Every function returns
// an empty string when nothing is found, so callers degrade gracefully.

#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <filesystem>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

#ifndef GPU_NAM_DEFAULT_MODEL_PATH
#define GPU_NAM_DEFAULT_MODEL_PATH ""
#endif

#ifndef GPU_NAM_ASSET_DIR
#define GPU_NAM_ASSET_DIR ""
#endif

namespace pulp::examples {

// Directory of the running module (executable or plugin dylib), for locating the
// bundled default model relative to the binary. Empty when unavailable.
inline std::string gpu_nam_module_dir() {
#if defined(__APPLE__)
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&gpu_nam_module_dir), &info) && info.dli_fname) {
        const std::string p = info.dli_fname;
        const auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#endif
    return {};
}

// Resolve the default `.nam` model: an explicit GPU_NAM_MODEL override, then the
// copy bundled into the plugin's Resources (relative to the binary), then the
// source-tree copy baked in at build time. Returns the first that exists.
inline std::string gpu_nam_default_model_path() {
    auto exists = [](const std::string& p) {
        if (p.empty()) return false;
        std::ifstream f(p, std::ios::binary);
        return static_cast<bool>(f);
    };
    if (const char* env = std::getenv("GPU_NAM_MODEL"); env && exists(env)) return env;
    const std::string dir = gpu_nam_module_dir();
    if (!dir.empty()) {
        // macOS bundle: Contents/MacOS/<bin> → Contents/Resources/example.nam.
        for (const std::string& rel : {"/../Resources/example.nam", "/example.nam"}) {
            const std::string p = dir + rel;
            if (exists(p)) return p;
        }
    }
    if (exists(GPU_NAM_DEFAULT_MODEL_PATH)) return GPU_NAM_DEFAULT_MODEL_PATH;
    return GPU_NAM_DEFAULT_MODEL_PATH;
}

// User-facing directory holding the installed sample `.nam` models + cabinet IRs
// (the installer writes them under "GPU NAM/Models" and "GPU NAM/Cabinets"). The
// file-chooser slots default here so a user testing the plugin lands on the
// samples without hunting. Resolution order: a GPU_NAM_CONTENT override, then a
// per-user copy under ~/Music or ~/Documents, then the system Application Support
// copy the installer creates. Empty when none exists.
//
// (This is a per-plugin convention prototyped here; a `pulp::platform` helper that
// returns the same per-OS location for any plugin would let the SDK own it.)
inline std::string gpu_nam_content_dir() {
    auto is_dir = [](const std::string& p) {
        std::error_code ec;
        return !p.empty() && std::filesystem::is_directory(p, ec);
    };
    if (const char* env = std::getenv("GPU_NAM_CONTENT"); env && is_dir(env)) return env;
    std::vector<std::string> cands;
    if (const char* home = std::getenv("HOME"); home && *home) {
        cands.push_back(std::string(home) + "/Music/GPU NAM");
        cands.push_back(std::string(home) + "/Documents/GPU NAM");
    }
    cands.push_back("/Library/Application Support/GPU NAM");
    for (const std::string& c : cands)
        if (is_dir(c)) return c;
    return {};
}

// A subdirectory of the content dir if it exists, else the content dir itself, else
// empty — used to point a file chooser at Models/ or Cabinets/.
inline std::string gpu_nam_content_subdir(const std::string& sub) {
    const std::string base = gpu_nam_content_dir();
    if (base.empty()) return {};
    std::error_code ec;
    const std::string p = base + "/" + sub;
    if (std::filesystem::is_directory(p, ec)) return p;
    return base;
}

// Filename (no directory) of a path, for display.
inline std::string gpu_nam_basename(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Resolve the vendored NAM UI-asset directory (images + fonts): an explicit
// GPU_NAM_ASSETS override, then the copy bundled into the plugin's Resources
// (relative to the binary), then the source-tree copy baked in at build time.
// Returns the first directory that exists; empty when none is found.
inline std::string gpu_nam_asset_dir() {
    auto dir_ok = [](const std::string& p) {
        if (p.empty()) return false;
        std::ifstream f(p + "/Background.jpg", std::ios::binary);
        return static_cast<bool>(f);
    };
    if (const char* env = std::getenv("GPU_NAM_ASSETS"); env && dir_ok(env)) return env;
    const std::string dir = gpu_nam_module_dir();
    if (!dir.empty()) {
        for (const std::string& rel : {"/../Resources/assets/nam", "/assets/nam"}) {
            const std::string p = dir + rel;
            if (dir_ok(p)) return p;
        }
    }
    if (dir_ok(GPU_NAM_ASSET_DIR)) return GPU_NAM_ASSET_DIR;
    return {};
}

}  // namespace pulp::examples
