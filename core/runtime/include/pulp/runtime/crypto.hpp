#pragma once

// Cryptographic primitives via Mbed TLS.
// SHA-256, MD5, AES-CBC, RSA sign/verify.

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>

namespace pulp::runtime {

// ── Hashing ─────────────────────────────────────────────────────────────

/// Compute SHA-256 hash. Returns 32-byte digest.
std::vector<uint8_t> sha256(const uint8_t* data, size_t size);
std::vector<uint8_t> sha256(std::string_view data);

/// Compute SHA-256 hash as hex string (64 chars).
std::string sha256_hex(const uint8_t* data, size_t size);
std::string sha256_hex(std::string_view data);

/// Compute MD5 hash. Returns 16-byte digest. (Legacy use only.)
std::vector<uint8_t> md5(const uint8_t* data, size_t size);
std::vector<uint8_t> md5(std::string_view data);

/// Compute MD5 hash as hex string (32 chars).
std::string md5_hex(std::string_view data);

// ── AES ─────────────────────────────────────────────────────────────────

/// AES-256-CBC encrypt. Key must be 32 bytes, IV 16 bytes.
/// Returns ciphertext (PKCS7 padded).
std::optional<std::vector<uint8_t>> aes_encrypt(
    const uint8_t* plaintext, size_t size,
    const uint8_t* key_32, const uint8_t* iv_16);

/// AES-256-CBC decrypt. Returns plaintext (PKCS7 unpadded).
std::optional<std::vector<uint8_t>> aes_decrypt(
    const uint8_t* ciphertext, size_t size,
    const uint8_t* key_32, const uint8_t* iv_16);

// ── Machine ID ──────────────────────────────────────────────────────────

/// Generate a machine-specific fingerprint (deterministic per machine).
/// Uses hardware identifiers (MAC address, CPU info, hostname) hashed with SHA-256.
std::string machine_id();

}  // namespace pulp::runtime
