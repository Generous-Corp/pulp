#pragma once

/// @file capsule.hpp
/// The admission entry point: open, preview, admit, extract, publish.
///
/// The order is the point. Nothing is extracted before the manifest is
/// validated, nothing is validated semantically before it is extracted into a
/// private directory, and nothing is compiled before a person has agreed to it.

#include <pulp/authoring_capsule/archive.hpp>
#include <pulp/authoring_capsule/limits.hpp>
#include <pulp/authoring_capsule/manifest.hpp>
#include <pulp/authoring_capsule/preview.hpp>
#include <pulp/authoring_capsule/profile_registry.hpp>
#include <pulp/authoring_capsule/signature.hpp>
#include <pulp/authoring_capsule/staging.hpp>
#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

struct AdmissionOptions {
    CapsuleLimits limits = kCapsuleLimitsV1;
    /// Optional; when absent, a signed capsule is admitted as unsigned rather
    /// than trusted. Absence of a verifier is never treated as verification.
    const SignatureVerifier* verifier = nullptr;
    /// Product identifier of the caller, used to distinguish "not for this
    /// product" from "not supported anywhere".
    std::string product;
};

/// Steps 1-4 of the import state machine: bounded manifest read, structural
/// and closure validation, optional trust envelope, and a non-executing
/// preview. Nothing is written outside the process.
///
/// This is also where the profile's `required_roles()` is applied — a missing
/// role surfaces as `missing_required_role` in `unmet` with an `unsupported`
/// verdict, which `admit_to_staging()` then refuses on. A consumer does not
/// repeat that check.
runtime::Result<CapsulePreview, CapsuleError> preview_capsule(const CapsuleArchive& archive,
                                                              const ProfileRegistry& registry,
                                                              const AdmissionOptions& options);

/// Steps 6-7: extract only declared members into `staging`, verifying digests,
/// then run the profile's semantic validation. Does not publish and does not
/// compile. The caller has consent before calling this.
runtime::Result<void, CapsuleError> admit_to_staging(const CapsuleArchive& archive,
                                                     const CapsulePreview& preview,
                                                     const ProfileRegistry& registry,
                                                     const StagingArea& staging,
                                                     const ExtractionProgress& progress = {});

/// One member of an export inventory. The exporter is handed exactly what may
/// travel; it never walks a project directory. That is what makes it
/// structurally impossible for an editor backup, a probe file, a cache, a log,
/// an absolute path, or a credential to end up in a capsule.
struct ExportItem {
    FileEntry entry;
    std::vector<std::uint8_t> bytes;
};

struct ExportRequest {
    Manifest manifest;
    std::vector<ExportItem> items;
};

/// Fill in the closure — sorted `files`, digests, sizes, and `revision_id` —
/// and write one deterministic archive to a destination that must not exist.
/// Repeating this for unchanged content reproduces the file byte for byte.
///
/// What it validates, exactly. Each item's path is re-admitted and may not
/// claim `capsule.json`; each digest and size is *measured* from the bytes
/// that travel rather than taken from what the caller declared; the item set
/// is checked for case and confusable collisions; and the finished manifest is
/// read back through `parse_manifest()` before the archive is created. That
/// last step is what applies the structural rules — a `files[]` row must
/// declare `source_availability: included`, a dependency's `provider` must be
/// `https://` or `capsule-library:`, `format`/`profile`/`product`/
/// `project_id` must be present — to a `Manifest` that was assembled in memory
/// and never parsed. `to_canonical_json()` alone applies none of them, so
/// without the read-back a caller passing its own dependency rows straight
/// through could mint a capsule this same code refuses to read. The refusal
/// carries `parse_manifest()`'s status and JSON-pointer subject, and nothing
/// is written.
///
/// `dependencies[]` is otherwise carried through as given: this layer resolves
/// nothing, contacts no provider, and cannot confirm that a dependency's
/// digest names the bytes a recipient will eventually fetch.
runtime::Result<std::uint64_t, CapsuleError>
export_capsule(ExportRequest request, const std::filesystem::path& destination,
               const CapsuleLimits& limits = kCapsuleLimitsV1);

}  // namespace pulp::authoring_capsule
