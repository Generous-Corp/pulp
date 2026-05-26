// install_paths_mac.hpp — macOS plugin install destinations + idempotency helpers
//
// Item 7.4b (macos-plugin-authoring-plan): `pulp build --install` wires
// build → validate → install. This header owns the side that decides
// "where does each plugin bundle land on disk?" and "is there an
// existing install we need to swap out atomically?".
//
// The header is deliberately free of cli_common.hpp so the unit test
// (`pulp-test-cli-install-paths-mac`) can compile this TU + the test
// without the full CLI runtime link surface. All I/O is mediated by a
// `InstallEnv` interface so the acceptance scenarios run deterministically
// without touching `~/Library/Audio/Plug-Ins/`.
//
// Per CLAUDE.md "Plugin Install Policy" the install only happens after
// validation passes — that policy decision lives in `cmd_build.cpp`;
// this module is the pure path-resolution + filesystem-swap surface.
//
// CLAUDE.md system folders (per-user installs):
//   AU   → ~/Library/Audio/Plug-Ins/Components/
//   VST3 → ~/Library/Audio/Plug-Ins/VST3/
//   CLAP → ~/Library/Audio/Plug-Ins/CLAP/

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pulp::cli::install_paths_mac {

namespace fs = std::filesystem;

// One of the three supported plugin bundle kinds on macOS. The kind
// determines (a) the system folder under `~/Library/Audio/Plug-Ins/`
// and (b) the validator name (`auval` / `pluginval` / `clap-validator`).
enum class PluginKind {
    Unknown,
    AU,      // .component → Components/
    VST3,    // .vst3      → VST3/
    CLAP,    // .clap      → CLAP/
};

// Classify a bundle path by its extension. Returns Unknown when the
// extension isn't one of `.component` / `.vst3` / `.clap` — callers
// should skip Unknown entries with a clear log line instead of guessing.
PluginKind classify_bundle(const fs::path& bundle);

// Validator name for a given plugin kind. Returns "" for Unknown.
// Used by `cmd_build --install` to decide which validator to invoke
// before promoting the bundle into the system folder.
std::string validator_for_kind(PluginKind kind);

// Compose the system destination for a plugin bundle, given the user's
// home directory. Returns:
//   AU   → <home>/Library/Audio/Plug-Ins/Components/<basename>
//   VST3 → <home>/Library/Audio/Plug-Ins/VST3/<basename>
//   CLAP → <home>/Library/Audio/Plug-Ins/CLAP/<basename>
// Returns empty path for Unknown. `home` must be absolute; relative
// paths produce empty (caller should bail).
fs::path destination_for(PluginKind kind,
                         const fs::path& home,
                         const fs::path& bundle_basename);

// Injectable I/O surface — tests construct one with stubbed callbacks.
// Production builds wire `make_default_env()` to the real `std::filesystem`
// + `find_executable_in_path` helpers.
struct InstallEnv {
    // Returns true iff the path exists. Tests fake this to simulate
    // "no previous install" or "previous install in place".
    std::function<bool(const fs::path&)> path_exists;

    // Create the parent directory recursively. Returns true on success.
    // Production wires this to `fs::create_directories`.
    std::function<bool(const fs::path&)> create_directories;

    // Remove a directory tree (a plugin bundle is a directory).
    // Returns true on success. Tests track removed paths.
    std::function<bool(const fs::path&)> remove_all;

    // Copy `src` to `dst`, recursively. Returns true on success.
    // Production wires to `fs::copy(src, dst, recursive | copy_symlinks)`.
    std::function<bool(const fs::path& src, const fs::path& dst)> copy_recursive;

    // Look up an executable on PATH. Empty when not found.
    // Used to surface a helpful "install with brew" message when the
    // validator for the current kind is missing.
    std::function<fs::path(const std::string& tool_name)> resolve_in_path;

    // The user's home directory. Production reads `$HOME`. Tests inject
    // a tmp path so `~/Library/Audio/Plug-Ins/...` resolution stays
    // hermetic.
    fs::path home_dir;
};

// Build an `InstallEnv` backed by real `std::filesystem` + PATH lookups.
// Defined in install_paths_mac.cpp.
InstallEnv make_default_env();

// Result of a single install attempt — one bundle, one destination.
// `bundle_path` is the source under `build/`; `destination` is the
// final path under `~/Library/Audio/Plug-Ins/...`.
struct InstallResult {
    bool success = false;
    std::string error;          // empty when success == true
    fs::path bundle_path;
    fs::path destination;
    bool replaced_existing = false;  // true when we removed a prior install
};

// Atomically install one bundle:
//   1. mkdir -p destination.parent_path()
//   2. if destination exists → rm -rf destination (idempotency)
//   3. cp -R bundle → destination
// Each step is mediated through `env` so tests verify the swap order.
//
// Returns success=true iff all three steps complete. On failure the
// result holds an error string explaining which step failed.
InstallResult install_bundle(const InstallEnv& env,
                             const fs::path& bundle,
                             PluginKind kind);

// Helpful "how do I install the missing validator?" hint per kind.
// Returned strings are short single-liners suitable for stderr.
//   AU   → "Xcode Command Line Tools (`xcode-select --install`) provides auval"
//   VST3 → "Install pluginval via `brew install pluginval`"
//   CLAP → "Install clap-validator via `cargo install clap-validator`"
//   Unknown → ""
std::string validator_install_hint(PluginKind kind);

// Discover every plugin bundle under `build_dir` that we know how to
// install. Walks `build/VST3/`, `build/CLAP/`, and `build/AU/` and
// classifies each `.vst3` / `.clap` / `.component` entry. Returns a
// stable order (AU → VST3 → CLAP) so output is deterministic.
//
// Bundles with Unknown classification are silently skipped — they
// shouldn't exist under those directories anyway.
struct DiscoveredBundle {
    fs::path bundle_path;
    PluginKind kind;
};

std::vector<DiscoveredBundle> discover_bundles(const fs::path& build_dir);

}  // namespace pulp::cli::install_paths_mac
