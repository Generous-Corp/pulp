// package_registry_trust.hpp — Ed25519 verification of the remote package
// registry (registry.json) against a public key pinned in the CLI.
//
// The registry owner signs the exact bytes of registry.json with
// tools/scripts/sign_package_registry.py and publishes the result beside it as
// registry.json.sig. The CLI fetches both, recomputes the signed message from
// the registry bytes it received, and verifies the signature with the pinned
// key before the registry is parsed or cached as trusted.
//
// Signed message (domain-separated, like kit registry manifests):
//   "pulp-package-registry-sig-v1\n" + sha256_hex(registry bytes) + "\n"
// Signature file (two lines, trailing newline optional):
//   pulp-package-registry-sig-v1
//   <64-byte Ed25519 signature as 128 hex characters>

#pragma once

#include <string>
#include <string_view>

namespace pulp::cli::pkg {

inline constexpr std::string_view kRegistrySignatureSchema = "pulp-package-registry-sig-v1";

// Ed25519 public key (32 bytes, lowercase hex) that must have signed the
// remote registry. Empty until the registry owner pins one: run
// `sign_package_registry.py keygen`, paste the printed public key here, and
// publish registry.json.sig beside registry.json. While it is empty the CLI
// uses the remote registry unsigned and prints a notice; a pinned key turns
// on enforcement (unsigned or invalid registries are refused unless
// --allow-unsigned-registry is passed).
inline constexpr std::string_view kPinnedRegistrySignerPublicKeyHex = "";

// The bytes an Ed25519 signature over `registry_bytes` covers.
std::string registry_signed_message(std::string_view registry_bytes);

struct RegistrySignatureCheck {
    bool ok = false;
    std::string
        code; // verified | no-trust-root | missing-signature | malformed-signature | signature-mismatch
    std::string error; // user-facing reason when !ok
};

// Verify `signature_text` (the registry.json.sig contents) over
// `registry_bytes` with `pinned_public_key_hex`.
RegistrySignatureCheck verify_registry_signature(std::string_view registry_bytes,
                                                 std::string_view signature_text,
                                                 std::string_view pinned_public_key_hex);

} // namespace pulp::cli::pkg
