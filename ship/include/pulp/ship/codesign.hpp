#pragma once

#include <string>
#include <vector>
#include <optional>

namespace pulp::ship {

// Code signing status
struct SigningInfo {
    bool is_signed = false;
    bool is_valid = false;
    std::string identity;      // e.g., "Developer ID Application: ..."
    std::string team_id;
    bool is_notarized = false;
    std::string error;
};

// Check if a binary/bundle is code-signed
SigningInfo check_codesign(const std::string& path);

// Check if a binary has been notarized
bool check_notarization(const std::string& path);

// Sign a binary/bundle with a given identity
bool codesign(const std::string& path, const std::string& identity,
              const std::string& entitlements = "");

// Submit for notarization (async — returns request UUID)
std::optional<std::string> notarize_submit(const std::string& path,
                                           const std::string& apple_id,
                                           const std::string& team_id,
                                           const std::string& password);

// Check notarization status
struct NotarizationStatus {
    bool complete = false;
    bool success = false;
    std::string message;
};
NotarizationStatus notarize_check(const std::string& request_uuid);

// Staple the notarization ticket
bool notarize_staple(const std::string& path);

// List available signing identities
std::vector<std::string> list_signing_identities();

// ── Package creation ─────────────────────────────────────────────────────

// Create a macOS .pkg installer
bool create_pkg(const std::string& component_path,
                const std::string& output_path,
                const std::string& identifier,
                const std::string& version,
                const std::string& signing_identity = "");

// Create a macOS .dmg disk image with an Applications alias
bool create_dmg(const std::string& source_path,
                const std::string& output_path,
                const std::string& volume_name);

// Create a combined multi-format installer (.pkg) via productbuild
// components: list of {path, install_location} pairs
struct InstallComponent {
    std::string path;           // Path to .component, .vst3, .clap, or .app
    std::string install_location; // e.g. "/Library/Audio/Plug-Ins/VST3"
};

bool create_combined_pkg(const std::vector<InstallComponent>& components,
                         const std::string& output_path,
                         const std::string& identifier,
                         const std::string& version,
                         const std::string& signing_identity = "");

// ── Entitlements ────────────────────────────────────────────────────────

// Generate a default entitlements plist for audio plugins
// Includes: audio-input, network-client (for updates)
std::string default_audio_entitlements();

} // namespace pulp::ship
