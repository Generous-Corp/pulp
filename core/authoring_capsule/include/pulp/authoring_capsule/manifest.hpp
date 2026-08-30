#pragma once

/// @file manifest.hpp
/// The capsule envelope: the only authority for what a capsule is.
///
/// The file extension, the marketplace category, the title, and the tags are
/// descriptive. Routing reads `profile`, `profile_version`, `product`,
/// `authoring_kind`, `topology`, and `required_capabilities` — nothing else.

#include <pulp/authoring_capsule/component.hpp>
#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

/// The vendor-neutral format identifier. Constant for format version 1.
inline constexpr std::string_view kFormatId = "org.pulp.audio-authoring-capsule";
inline constexpr std::uint32_t kFormatVersion = 1;

/// Fixed path of the manifest, which must also be the archive's first member.
inline constexpr std::string_view kManifestPath = "capsule.json";

enum class Reproducibility : std::uint8_t {
    /// Declared inputs plus pinned dependencies regenerate the result, and
    /// receipts prove the claim.
    reproducible,
    /// Source and history are present, but a model, tool, or environment
    /// cannot be replayed exactly. Preserving the accepted output is still
    /// worth doing; claiming determinism it does not have is not.
    best_effort,
    /// Only playable derived artifacts are present.
    frozen_output_only,
};

/// Version floors the consuming runtime must satisfy. Checkable without
/// loading any code.
struct Compatibility {
    std::string min_product_version;
    std::string min_runtime_version;
    std::string schema_version;
    /// Additional profile-owned floors, as canonical JSON.
    std::string extra_json = "{}";
};

struct Manifest {
    std::string format = std::string(kFormatId);
    std::uint32_t format_version = kFormatVersion;

    // ── Routing. Authenticated, bounded, and the only routing input. ──
    std::string profile;
    std::uint32_t profile_version = 1;
    std::string product;
    std::string authoring_kind;
    std::vector<std::string> subtypes;
    /// Exact runtime topology, profile-owned canonical JSON.
    std::string topology_json = "{}";
    std::vector<std::string> required_capabilities;

    // ── Identity. ──
    /// Stable across saves of the same project.
    std::string project_id;
    /// Derived from canonical content; see `revision_digest()`.
    std::string revision_id;
    std::string parent_revision;

    Reproducibility reproducibility = Reproducibility::best_effort;
    Compatibility compatibility;
    std::vector<FileEntry> files;
    std::vector<DependencyEntry> dependencies;

    // ── Descriptive. Excluded from the revision digest. ──
    std::string title;
    std::string created_at;
    std::string exported_at;
    std::string provenance_json = "{}";
    std::string attestations_json = "[]";
    std::string distribution_json = "{}";

    /// Unknown optional keys, preserved verbatim in canonical form so a
    /// round-trip through an older reader does not silently drop metadata a
    /// newer writer emitted. An unknown *required* role or capability fails
    /// closed instead; see `CapsuleStatus::unsupported_capability`.
    std::string unknown_optional_json = "{}";
};

/// Serialize to canonical JSON: UTF-8 without BOM, LF, keys sorted by code
/// point, no insignificant whitespace, shortest round-tripping numbers, no
/// NaN or infinity, `files` sorted by path and `dependencies` by id in byte
/// order.
///
/// Fallible because a hand-assembled `Manifest` can hold material no reader
/// ever validated — a non-UTF-8 string, or a preserved subtree past the depth
/// limit. Returning an empty string instead would hand the exporter a capsule
/// that fails admission later, with nothing to tell the user why.
runtime::Result<std::string, CapsuleError> to_canonical_json(const Manifest& manifest);

/// Parse and structurally validate. Does not check the archive closure — that
/// needs the archive; see `admit()`.
runtime::Result<Manifest, CapsuleError> parse_manifest(std::string_view json);

/// `sha256` over the canonical JSON of the whole manifest with exactly three
/// fields removed: `revision_id` (self-referential), `exported_at` (the one
/// value that changes when nothing else did), and `attestations` (signatures
/// are computed over this digest).
///
/// Everything else is covered — `title`, `created_at`, `provenance`,
/// `distribution`, each file row's `executable_data`, and any unknown optional
/// key. Those fields are not inert: `distribution` is what makes a capsule
/// Play-only and `executable_data` is what tells the user a payload contains
/// code, so leaving either outside the digest would let outer metadata be
/// edited on a validly signed capsule and still verify.
///
/// None of the covered fields depends on the machine or the moment, so the
/// digest remains independent of compression level, archive member order,
/// filesystem timestamps, export time, and the exporting machine's paths. Two
/// exports of an unchanged project agree.
///
/// Fallible for the same reason `to_canonical_json()` is: the digest is taken
/// over canonical bytes, so it cannot exist when those bytes cannot.
runtime::Result<std::string, CapsuleError> revision_digest(const Manifest& manifest);

/// Recomputed from the component rows. A declared value in the capsule is
/// ignored.
Completeness derive_completeness(const Manifest& manifest);

}  // namespace pulp::authoring_capsule
