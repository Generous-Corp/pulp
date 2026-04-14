#pragma once

// Plugin Scanner for Pulp Host
// Scans standard system directories for VST3, AU, CLAP, and LV2 plugins.
// Returns a list of PluginInfo descriptors that can be used to load plugins.
//
// Usage:
//   auto scanner = PluginScanner();
//   auto plugins = scanner.scan();
//   for (auto& p : plugins) { ... p.name, p.path, p.format ... }

#include <string>
#include <vector>
#include <functional>

namespace pulp::host {

// ── Plugin format identifier ────────────────────────────────────────────

enum class PluginFormat {
    VST3,
    AudioUnit,     // AU v2 (macOS)
    AudioUnitV3,   // AUv3 (macOS/iOS)
    CLAP,
    LV2,
};

// ── Plugin descriptor from scanning ─────────────────────────────────────

struct PluginInfo {
    std::string name;
    std::string manufacturer;
    std::string version;
    std::string path;          // Full path to the plugin bundle/binary
    std::string unique_id;     // Format-specific unique ID (FUID, bundle ID, etc.)
    PluginFormat format;
    bool is_instrument = false;
    bool is_effect = true;
    int num_inputs = 2;
    int num_outputs = 2;

    // ── Richer metadata (workstream 03 slice 3.7) ────────────────────────
    //
    // Each scanner populates the subset it can cheaply extract. Callers
    // filter plugins on these instead of parsing `name` strings. All
    // fields are additive — older cache blobs that don't carry them
    // deserialize with defaults, so scan_cache.hpp's round-trip stays
    // backwards-compatible.
    //
    // category    — broad taxonomy: "Fx", "Instrument", "Analyzer",
    //               "MidiEffect". Sourced from VST3 PClassInfo::category,
    //               AU AUSysExCategory, CLAP descriptor category.
    // features    — free-form tags (CLAP `features` array, VST3
    //               subcategories joined, AU tag list).
    // description — longer human-readable description if the format
    //               surfaces one; empty otherwise.
    // has_editor  — plugin declares an embedded editor view.
    // supports_sidechain — declares a second input bus.
    // supports_midi_in / supports_midi_out — MIDI wiring.
    std::string category;
    std::vector<std::string> features;
    std::string description;
    bool has_editor = false;
    bool supports_sidechain = false;
    bool supports_midi_in = false;
    bool supports_midi_out = false;
};

// ── Scanner configuration ───────────────────────────────────────────────

struct ScanOptions {
    bool scan_vst3 = true;
    bool scan_au = true;
    bool scan_clap = true;
    bool scan_lv2 = true;
    std::vector<std::string> extra_paths;  // Additional scan directories

    // Callback for progress reporting during scan
    using ProgressCallback = std::function<void(const std::string& current_path, int scanned, int total)>;
    ProgressCallback on_progress;
};

// ── Plugin Scanner ──────────────────────────────────────────────────────

class PluginScanner {
public:
    PluginScanner() = default;

    // Scan all standard plugin directories and return found plugins.
    // This performs I/O and may take seconds on large collections.
    std::vector<PluginInfo> scan(const ScanOptions& options = {});

    // Get default scan paths for a specific format on the current platform
    static std::vector<std::string> default_paths(PluginFormat format);

    // Quick check if a specific path looks like a valid plugin bundle
    static bool is_plugin_bundle(const std::string& path, PluginFormat format);

private:
    std::vector<PluginInfo> scan_directory(const std::string& dir, PluginFormat format);
    PluginInfo scan_vst3_bundle(const std::string& path);
    PluginInfo scan_clap_bundle(const std::string& path);
    PluginInfo scan_lv2_bundle(const std::string& path);
#ifdef __APPLE__
    std::vector<PluginInfo> scan_audio_units();
#endif
};

} // namespace pulp::host
