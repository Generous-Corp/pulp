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

/// Compute SHA-1 hash. Returns 20-byte digest. (Legacy protocols only —
/// e.g., WebSocket handshake per RFC 6455.)
std::vector<uint8_t> sha1(const uint8_t* data, size_t size);
std::vector<uint8_t> sha1(std::string_view data);

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

// ── HMAC (RFC 2104 / RFC 2202 / RFC 4231) ───────────────────────────────

/// Compute HMAC-SHA256(key, data). Returns 32-byte tag on success,
/// `std::nullopt` if the underlying mbedTLS context fails (alloc /
/// bad input / build-config mismatch — rare in practice).
/// Vectors: RFC 4231 (HMAC-SHA-256).
std::optional<std::vector<uint8_t>> hmac_sha256(
    const uint8_t* key, size_t key_size,
    const uint8_t* data, size_t data_size);
std::optional<std::vector<uint8_t>> hmac_sha256(std::string_view key,
                                                  std::string_view data);

/// Compute HMAC-SHA1(key, data). Returns 20-byte tag on success,
/// `std::nullopt` on mbedTLS failure (see hmac_sha256). Legacy use
/// (e.g., AWS Signature v2).
/// Vectors: RFC 2202 (HMAC-SHA-1).
std::optional<std::vector<uint8_t>> hmac_sha1(
    const uint8_t* key, size_t key_size,
    const uint8_t* data, size_t data_size);

// ── AES-GCM (AEAD, NIST SP 800-38D) ─────────────────────────────────────

/// Output of AES-GCM encryption: ciphertext + 16-byte authentication tag.
struct GcmOutput {
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> tag; ///< 16 bytes
};

/// AES-256-GCM encrypt with optional Additional Authenticated Data (AAD).
/// IV/nonce should be 12 bytes (96 bits) per NIST SP 800-38D §8.2.1.
/// Returns ciphertext (same length as plaintext) + 16-byte authentication tag.
/// Returns std::nullopt on internal mbedTLS failure (rare; mostly OOM).
std::optional<GcmOutput> aes_gcm_encrypt(
    const uint8_t* plaintext, size_t plaintext_size,
    const uint8_t* key_32,
    const uint8_t* iv_12, size_t iv_size,
    const uint8_t* aad, size_t aad_size);

/// AES-256-GCM decrypt + verify the 16-byte tag.
/// Returns plaintext on success; std::nullopt if the tag fails to verify
/// (caller must treat that as a hard failure — *do not* surface partial
/// plaintext on tag mismatch; the buffer is overwritten regardless but
/// the contract is "verified or null").
std::optional<std::vector<uint8_t>> aes_gcm_decrypt(
    const uint8_t* ciphertext, size_t ciphertext_size,
    const uint8_t* key_32,
    const uint8_t* iv_12, size_t iv_size,
    const uint8_t* aad, size_t aad_size,
    const uint8_t* tag_16);

// ── Constant-time comparison ────────────────────────────────────────────

/// Constant-time byte comparison. Returns true iff @p a and @p b match.
/// Use whenever comparing MAC tags, key material, or other secret bytes
/// — `std::memcmp` short-circuits and leaks timing information.
bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t size);

// ── Ed25519 (RFC 8032) ──────────────────────────────────────────────────
//
// Detached EdDSA signing over the Ed25519 curve. Used by:
//   - Sparkle appcast `edSignature` (macOS auto-update)
//   - Future v2 license-key payloads (post-RSA migration)
//
// Implementation: vendored TweetNaCl reference (public domain, RFC 8032
// compliant). TweetNaCl exposes a combined sign-and-message API; the
// wrappers here expose a detached 64-byte signature for direct
// interoperability with Sparkle and most ecosystem tools.

/// Ed25519 key sizes (RFC 8032 §5.1).
inline constexpr size_t ed25519_public_key_size = 32;
inline constexpr size_t ed25519_private_key_size = 64;  // seed (32) + public key (32)
inline constexpr size_t ed25519_seed_size = 32;
inline constexpr size_t ed25519_signature_size = 64;

/// Ed25519 keypair (NaCl convention: secret key contains seed || public key).
struct Ed25519KeyPair {
    std::vector<uint8_t> public_key;   ///< 32 bytes
    std::vector<uint8_t> private_key;  ///< 64 bytes (seed || public_key)
};

/// Generate a fresh Ed25519 keypair using mbedTLS CTR-DRBG for entropy.
/// Returns std::nullopt on RNG failure (rare; reserved for hardware-RNG
/// failure on locked-down platforms).
std::optional<Ed25519KeyPair> ed25519_keypair_generate();

/// Derive a deterministic Ed25519 keypair from a 32-byte seed.
/// Useful for test vectors and for reproducing a key from a backed-up seed.
std::optional<Ed25519KeyPair> ed25519_keypair_from_seed(
    const uint8_t* seed_32, size_t seed_size);

/// Sign a message with an Ed25519 private key (64-byte NaCl-form).
/// Returns the 64-byte detached signature, or std::nullopt on input
/// size validation failure.
std::optional<std::vector<uint8_t>> ed25519_sign(
    const uint8_t* private_key_64, size_t private_key_size,
    const uint8_t* message, size_t message_size);

/// Verify a 64-byte detached signature against a message + 32-byte public key.
/// Returns true iff the signature is authentic. Returns false on any input-
/// size mismatch (defence in depth — callers should still range-check).
bool ed25519_verify(
    const uint8_t* public_key_32, size_t public_key_size,
    const uint8_t* signature_64, size_t signature_size,
    const uint8_t* message, size_t message_size);

// ── Machine ID ──────────────────────────────────────────────────────────

/// Generate a machine-specific fingerprint (deterministic per machine).
/// Uses hardware identifiers (MAC address, CPU info, hostname) hashed with SHA-256.
std::string machine_id();

}  // namespace pulp::runtime
