#include <pulp/authoring_capsule/status.hpp>

namespace pulp::authoring_capsule {

std::string_view status_token(CapsuleStatus status) noexcept {
    switch (status) {
        case CapsuleStatus::ok: return "ok";
        case CapsuleStatus::manifest_invalid: return "manifest_invalid";
        case CapsuleStatus::manifest_not_first: return "manifest_not_first";
        case CapsuleStatus::unsupported_format: return "unsupported_format";
        case CapsuleStatus::unsupported_format_version: return "unsupported_format_version";
        case CapsuleStatus::unsupported_profile: return "unsupported_profile";
        case CapsuleStatus::unsupported_profile_version: return "unsupported_profile_version";
        case CapsuleStatus::unsupported_product: return "unsupported_product";
        case CapsuleStatus::unsupported_capability: return "unsupported_capability";
        case CapsuleStatus::missing_required_role: return "missing_required_role";
        case CapsuleStatus::runtime_floor_too_old: return "runtime_floor_too_old";
        case CapsuleStatus::schema_migration_refused: return "schema_migration_refused";
        case CapsuleStatus::closure_violation: return "closure_violation";
        case CapsuleStatus::digest_mismatch: return "digest_mismatch";
        case CapsuleStatus::unsafe_archive: return "unsafe_archive";
        case CapsuleStatus::archive_budget_exceeded: return "archive_budget_exceeded";
        case CapsuleStatus::path_rejected: return "path_rejected";
        case CapsuleStatus::path_collision: return "path_collision";
        case CapsuleStatus::missing_dependency: return "missing_dependency";
        case CapsuleStatus::dependency_digest_mismatch: return "dependency_digest_mismatch";
        case CapsuleStatus::dependency_provider_denied: return "dependency_provider_denied";
        case CapsuleStatus::missing_licensed_sample: return "missing_licensed_sample";
        case CapsuleStatus::rights_insufficient: return "rights_insufficient";
        case CapsuleStatus::signature_invalid: return "signature_invalid";
        case CapsuleStatus::revoked_signer: return "revoked_signer";
        case CapsuleStatus::downgrade_refused: return "downgrade_refused";
        case CapsuleStatus::creator_identity_required: return "creator_identity_required";
        case CapsuleStatus::decode_unsupported: return "decode_unsupported";
        case CapsuleStatus::staging_failed: return "staging_failed";
        case CapsuleStatus::publication_conflict: return "publication_conflict";
        case CapsuleStatus::cancelled: return "cancelled";
    }
    return "manifest_invalid";
}

}  // namespace pulp::authoring_capsule
