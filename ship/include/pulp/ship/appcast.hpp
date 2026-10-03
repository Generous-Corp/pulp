#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace pulp::ship {

// Represents a single release in an appcast feed
struct AppcastItem {
    std::string version;           // e.g., "1.2.0"
    std::string build_number;      // e.g., "42"
    std::string title;             // e.g., "Version 1.2.0"
    std::string description;       // HTML release notes
    std::string pub_date;          // RFC 2822: "Mon, 25 Mar 2026 12:00:00 +0000"
    std::string download_url;      // URL to the installer/archive
    uint64_t file_size = 0;        // In bytes
    std::string ed_signature;      // EdDSA signature (base64)
    std::string minimum_os;        // e.g., "12.0" for macOS Monterey

    // Sparkle 2 enclosure installer kind. "package" marks a flat .pkg /
    // .mpkg (Sparkle runs it with /usr/sbin/installer after an administrator
    // prompt). Empty omits the attribute, which Sparkle treats as an archived
    // app bundle. `pulp ship appcast` fills this in for a .pkg/.mpkg URL.
    std::string installation_type;
    // <sparkle:releaseNotesLink>: an HTML page Sparkle loads into the update
    // dialog instead of <description>. Prefer inline `description` for assets
    // hosted where the server sends `Content-Disposition: attachment`
    // (GitHub release assets do), because the dialog's web view then has
    // nothing to render.
    std::string release_notes_link;
    // <sparkle:fullReleaseNotesLink>: the "Version History" page Sparkle
    // opens in the user's browser (for example the GitHub release page).
    std::string full_release_notes_link;
    // <sparkle:channel>: items on a named channel are offered only to apps
    // whose updater delegate allows that channel. Empty = default channel,
    // which every client sees.
    std::string channel;
};

// An appcast feed (Sparkle-compatible XML)
struct Appcast {
    std::string title;             // Feed title, e.g., "PulpGain Updates"
    std::string link;              // Feed URL
    std::string description;       // Feed description
    std::vector<AppcastItem> items;

    // Generate Sparkle-compatible appcast XML
    std::string to_xml() const;

    // Parse an appcast XML string
    static std::optional<Appcast> from_xml(const std::string& xml);
};

// The Sparkle installation type for an update artifact, judged from its file
// name: "package" for .pkg / .mpkg (case-insensitive), empty otherwise.
std::string sparkle_installation_type_for(const std::string& artifact_path);

// ── Version comparison ───────────────────────────────────────────────────────

// Compare semantic versions. Returns: -1 if a < b, 0 if equal, 1 if a > b
int compare_versions(const std::string& a, const std::string& b);

// ── EdDSA signing for Sparkle ────────────────────────────────────────────────

// Generate an EdDSA (Ed25519) key pair for update signing
// Returns base64-encoded private key, public key
struct KeyPair {
    std::string private_key_b64;
    std::string public_key_b64;
};

// Sign a file with Ed25519 and return a base64-encoded 64-byte detached
// signature suitable for the `sparkle:edSignature` attribute on an
// `<enclosure>` element.
//
// Accepts either a 32-byte Ed25519 seed (Sparkle's modern `generate_keys`
// output) or a 64-byte NaCl-form secret key (seed || public_key), both
// base64-encoded. Backed by `pulp::runtime::ed25519_sign()` (TweetNaCl
// reference implementation, RFC 8032).
//
// Returns std::nullopt when signing is unavailable OR when inputs are
// invalid (unreadable file, malformed key). Callers MUST treat nullopt
// as a hard failure and refuse to produce an unsigned appcast. Returning
// an empty string silently looks like a successful sign to the CLI but
// emits `edSignature=""` into the appcast, which Sparkle then parses as
// "not signed yet" while operators believe they shipped a signed update.
std::optional<std::string> sign_file_ed25519(const std::string& file_path,
                                             const std::string& private_key_b64);

// Verify a base64-encoded 64-byte detached Ed25519 signature against a
// file's raw bytes and a base64-encoded 32-byte public key. Returns true
// iff the signature is authentic.
bool verify_file_ed25519(const std::string& file_path,
                         const std::string& signature_b64,
                         const std::string& public_key_b64);

// Verify every `<enclosure sparkle:edSignature="...">` item in an appcast
// against the supplied base64 public key. Items without `ed_signature`
// are skipped. Each item's `download_url` is treated as a local file
// path (HTTP URLs must be downloaded by the caller first). Returns true
// iff every signed item verifies AND at least one item carried a
// signature.
bool verify_appcast_signatures(const Appcast& feed,
                               const std::string& public_key_b64);

} // namespace pulp::ship
