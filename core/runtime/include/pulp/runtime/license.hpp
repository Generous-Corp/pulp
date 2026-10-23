#pragma once

// License key validation — plugin licensing.
//
// Format versions:
//   v1 — legacy: base64(json).base64(RSA_SHA256(json)). Asymmetric;
//        server holds an RSA private key, plugin embeds the public PEM.
//        Continues to validate for one release window for backward compat.
//   v2 — current: "v2." + base64url(IV || AES-256-GCM(json) || tag).
//        Symmetric; server + plugin share a 32-byte secret. Smaller keys,
//        no PEM parsing, and the GCM tag covers integrity (eliminates the
//        separate signature). HMAC-SHA256 is still available as a standalone
//        primitive for use cases (e.g. activation tokens) that need MAC
//        without payload encryption.

#include <pulp/runtime/crypto.hpp>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <cstdint>

namespace pulp::runtime {

/// License-key format version. v1 is the legacy RSA-signed format; v2
/// is the AES-256-GCM AEAD format introduced for the macOS 1.0 release.
enum class LicenseFormatVersion {
    V1 = 1,  ///< base64(json).base64(RSA_sig) — legacy
    V2 = 2,  ///< "v2." + base64url(IV || AES-256-GCM(json) || tag)
};

/// Latest format emitted by `LicenseGenerator`. Bumped each time the
/// on-disk format changes; old formats stay parseable.
inline constexpr LicenseFormatVersion kCurrentLicenseFormat =
    LicenseFormatVersion::V2;

/// License key status
enum class LicenseStatus {
    Valid,
    Expired,
    InvalidSignature,
    InvalidFormat,
    MachineIdMismatch,
    NotFound
};

/// Parsed license key data
struct LicenseInfo {
    std::string product_id;
    std::string user_email;
    std::string machine_id;    // Empty = any machine
    int64_t issued_timestamp = 0;
    int64_t expiry_timestamp = 0;  // 0 = perpetual
    std::string edition;       // "standard", "pro", etc.
};

/// License key validator — checks v1 RSA-signed or v2 AES-256-GCM keys.
/// The public key (v1) and shared secret (v2) are embedded in the plugin;
/// the matching private key (v1) and shared secret (v2) stay on the server.
class LicenseValidator {
public:
    LicenseValidator() = default;

    /// Set the RSA public key (PEM format) for v1 signature verification.
    /// Optional if the plugin no longer ships v1 keys.
    void set_public_key(std::string_view pem);

    /// Set the 32-byte symmetric secret used to decrypt v2 license keys.
    /// Optional if the plugin still only ships v1 keys.
    void set_shared_secret(const uint8_t* secret_32, size_t secret_size);

    /// Validate a license key string. Auto-detects v1 vs v2 via
    /// `detect_license_format()` and dispatches to the matching path.
    LicenseStatus validate(std::string_view license_key) const;

    /// Validate and extract license info.
    std::optional<LicenseInfo> validate_and_parse(std::string_view license_key) const;

    /// Validate a license key file.
    LicenseStatus validate_file(std::string_view path) const;

    /// Check if a license is valid for this machine.
    bool is_valid_for_machine(const LicenseInfo& info) const;

private:
    std::string public_key_pem_;
    std::vector<uint8_t> shared_secret_;

    std::optional<LicenseInfo> parse_payload(std::string_view json) const;
    bool verify_signature(std::string_view payload, std::string_view signature_b64) const;

    LicenseStatus validate_v1(std::string_view license_key) const;
    LicenseStatus validate_v2(std::string_view license_key) const;
};

/// License key generator (server-side utility).
/// Uses an RSA private key to sign license data (legacy v1) or a
/// 32-byte symmetric secret (v2 AES-256-GCM).
class LicenseGenerator {
public:
    LicenseGenerator() = default;

    /// Set the RSA private key (PEM format) for signing v1 license keys.
    /// Optional — only required if you call `generate_v1()` directly.
    void set_private_key(std::string_view pem);

    /// Set the 32-byte symmetric secret for v2 AES-256-GCM license keys.
    /// This same secret must be embedded in the plugin via
    /// `LicenseValidator::set_shared_secret()`.
    void set_shared_secret(const uint8_t* secret_32, size_t secret_size);

    /// Generate a license key string in the current format
    /// (`kCurrentLicenseFormat`). Selects between v1 (RSA) and v2
    /// (AES-256-GCM) based on which key has been set.
    std::optional<std::string> generate(const LicenseInfo& info) const;

    /// Force v1 (RSA-signed) license output. Used by migration tools
    /// that need to emit a legacy key for compatibility testing.
    std::optional<std::string> generate_v1(const LicenseInfo& info) const;

    /// Force v2 (AES-256-GCM) license output. Used by `migrate_v1_to_v2`.
    std::optional<std::string> generate_v2(const LicenseInfo& info) const;

private:
    std::string private_key_pem_;
    std::vector<uint8_t> shared_secret_;
};

// ── Format helpers and migration ────────────────────────────────────────

/// Detect the format version of a license key without parsing the payload.
/// Returns std::nullopt if the string matches no known format.
std::optional<LicenseFormatVersion> detect_license_format(
    std::string_view license_key);

/// Re-encode a v1 license key as v2 using the supplied 32-byte secret.
///
/// The v1 RSA-signature is verified before re-emission (so a v1 key that
/// fails verification cannot be "migrated" to a v2 key that does pass).
/// The exact LicenseInfo (product_id, email, machine_id, edition, issued,
/// expiry) is preserved.
///
/// Use case: a server holds historical v1 keys; on next renewal the user
/// downloads a freshly-issued v2 key. For self-service migration the
/// plugin itself can call this helper with the locally-cached v2 secret.
std::optional<std::string> migrate_v1_to_v2(
    std::string_view v1_license_key,
    std::string_view v1_public_key_pem,
    const uint8_t* v2_secret_32,
    size_t v2_secret_size);

/// Transport policy for OnlineActivation.
struct OnlineActivationOptions {
    /// Permit a plain `http://` server_url. Off by default: every exchange
    /// carries the serial or license key plus the machine id, and the status
    /// verdict is acted on, so only a loopback test server should be reached
    /// without TLS.
    bool allow_insecure_http = false;
    int timeout_seconds = 30;
};

/// Online license activation flow.
/// Every exchange is a JSON POST to an https license server; the serial,
/// license key and machine id travel in the request body, never in the URL.
struct OnlineActivation {
    /// Activate online with a serial number.
    /// Returns the license key on success.
    static std::optional<std::string> activate(
        std::string_view server_url,
        std::string_view serial_number,
        std::string_view product_id,
        const OnlineActivationOptions& options = {});

    /// Deactivate a license (free up a machine slot).
    static bool deactivate(
        std::string_view server_url,
        std::string_view license_key,
        const OnlineActivationOptions& options = {});

    /// Check license status with the server. Returns NotFound when the server
    /// is refused or unreachable, and InvalidSignature when it answered but did
    /// not vouch for the key (see parse_status_response).
    static LicenseStatus check_status(
        std::string_view server_url,
        std::string_view license_key,
        const OnlineActivationOptions& options = {});

    /// True when `server_url` is an `https://` origin, or `http://` and the
    /// options allow it. Nothing is sent to a URL this rejects.
    static bool server_url_allowed(std::string_view server_url,
                                   const OnlineActivationOptions& options = {});

    /// Exact parse of a `/status` response body: a JSON object whose `status`
    /// member is the string "valid" or "expired" maps to Valid / Expired;
    /// anything else (other values, other fields, non-objects, malformed JSON)
    /// is InvalidSignature. The word "valid" appearing elsewhere in the body
    /// is not a verdict.
    static LicenseStatus parse_status_response(std::string_view body);
};

}  // namespace pulp::runtime
