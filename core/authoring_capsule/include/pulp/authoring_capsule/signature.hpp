#pragma once

/// @file signature.hpp
/// Trust is an adapter, not a dependency.
///
/// The substrate does not link a key store, a revocation feed, or a
/// marketplace client. A consumer supplies one of these and keeps its own
/// policy. That also keeps the honest boundary visible in code: a valid
/// signature answers *who*, and nothing else. It never authorizes execution,
/// never grants a redistribution right, and never turns an immutable Play
/// install into an editable project.

#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

struct SignatureEnvelope {
    /// Signer identity as the verifier understands it.
    std::string signer_id;
    std::string algorithm;
    std::vector<std::uint8_t> signature;
    /// Canonical JSON of what was signed: the manifest's semantic subtree plus
    /// the revision digest. Signing descriptive metadata would let a title
    /// change invalidate a signature, or worse, let outer metadata be swapped
    /// under a valid one.
    std::string signed_payload;
};

/// Minimum version the signer will let this capsule be admitted at. Present so
/// a downgrade to a revoked-and-fixed revision is refusable.
struct VersionFloor {
    std::string product;
    std::string min_version;
};

class SignatureVerifier {
public:
    virtual ~SignatureVerifier() = default;

    /// Verify the envelope against the digest that was actually computed from
    /// the capsule, not against a digest the capsule asserts about itself.
    virtual runtime::Result<void, CapsuleError>
    verify(const SignatureEnvelope& envelope, std::string_view computed_revision_digest) const = 0;

    /// Has this signer been revoked? A revoked signer is a distinct result
    /// from an invalid signature: the bytes are intact and the identity is
    /// real, which is exactly why the user needs to be told the difference.
    virtual bool is_revoked(std::string_view signer_id) const = 0;

    virtual runtime::Result<void, CapsuleError> check_floor(const VersionFloor& floor) const = 0;
};

}  // namespace pulp::authoring_capsule
