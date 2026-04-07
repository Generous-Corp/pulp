#pragma once

// License key validation — RSA signature verification for plugin licensing.
// Provides key file generation (server-side), validation (client-side),
// and an online unlock flow.

#include <pulp/runtime/crypto.hpp>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <cstdint>

namespace pulp::runtime {

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

/// License key validator — checks RSA-signed license files.
/// The public key is embedded in the plugin; the private key stays on the server.
class LicenseValidator {
public:
    LicenseValidator() = default;

    /// Set the RSA public key (PEM format) for signature verification.
    void set_public_key(std::string_view pem);

    /// Validate a license key string.
    /// The key is: base64(json_payload) + "." + base64(rsa_signature)
    LicenseStatus validate(std::string_view license_key) const;

    /// Validate and extract license info.
    std::optional<LicenseInfo> validate_and_parse(std::string_view license_key) const;

    /// Validate a license key file.
    LicenseStatus validate_file(std::string_view path) const;

    /// Check if a license is valid for this machine.
    bool is_valid_for_machine(const LicenseInfo& info) const;

private:
    std::string public_key_pem_;

    std::optional<LicenseInfo> parse_payload(std::string_view json) const;
    bool verify_signature(std::string_view payload, std::string_view signature_b64) const;
};

/// License key generator (server-side utility).
/// Uses an RSA private key to sign license data.
class LicenseGenerator {
public:
    LicenseGenerator() = default;

    /// Set the RSA private key (PEM format) for signing.
    void set_private_key(std::string_view pem);

    /// Generate a signed license key string.
    std::optional<std::string> generate(const LicenseInfo& info) const;

private:
    std::string private_key_pem_;
};

/// Online license activation flow.
/// POST to a license server, receive and store a license key.
struct OnlineActivation {
    /// Activate online with a serial number.
    /// Returns the license key on success.
    static std::optional<std::string> activate(
        std::string_view server_url,
        std::string_view serial_number,
        std::string_view product_id);

    /// Deactivate a license (free up a machine slot).
    static bool deactivate(
        std::string_view server_url,
        std::string_view license_key);

    /// Check license status with the server.
    static LicenseStatus check_status(
        std::string_view server_url,
        std::string_view license_key);
};

}  // namespace pulp::runtime
