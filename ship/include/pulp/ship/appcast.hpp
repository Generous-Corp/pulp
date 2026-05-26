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
// an empty string silently was the #295 P0 bug — it looked like a
// successful sign to the CLI but emitted `edSignature=""` into the
// appcast, which Sparkle then parsed as "not signed yet" while operators
// believed they had shipped a signed update.
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
