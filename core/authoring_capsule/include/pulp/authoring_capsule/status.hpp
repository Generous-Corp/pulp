#pragma once

/// @file status.hpp
/// Admission failure vocabulary for authoring capsules.
///
/// Every rejection resolves to exactly one `CapsuleStatus`. A generic failure
/// is a defect: the product needs to name the missing runtime, the unresolved
/// dependency, or the offending member so a person can act on it.

#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::authoring_capsule {

enum class CapsuleStatus : std::uint8_t {
    ok,

    // Manifest and versioning.
    manifest_invalid,
    manifest_not_first,
    unsupported_format,
    unsupported_format_version,
    unsupported_profile,
    unsupported_profile_version,
    unsupported_product,
    unsupported_capability,
    runtime_floor_too_old,
    schema_migration_refused,

    // Archive admission.
    closure_violation,
    digest_mismatch,
    unsafe_archive,
    archive_budget_exceeded,
    path_rejected,
    path_collision,

    // Dependencies and rights.
    missing_dependency,
    dependency_digest_mismatch,
    dependency_provider_denied,
    missing_licensed_sample,
    rights_insufficient,

    // Trust.
    signature_invalid,
    revoked_signer,
    downgrade_refused,
    creator_identity_required,

    // Media.
    decode_unsupported,

    // Publication.
    staging_failed,
    publication_conflict,
    cancelled,
};

/// Stable machine-readable token, e.g. `"digest_mismatch"`. Stable across
/// releases: products key their explanatory copy off these.
std::string_view status_token(CapsuleStatus status) noexcept;

/// A rejection with the exact subject that caused it.
struct CapsuleError {
    CapsuleStatus status = CapsuleStatus::manifest_invalid;
    /// The member path, dependency id, capability name, profile id, or field
    /// pointer the failure is about. Never a message for a human to read.
    std::string subject;
    /// What was required, when the failure is a version or floor mismatch.
    std::string required;
    /// What was present instead.
    std::string found;
};

}  // namespace pulp::authoring_capsule
