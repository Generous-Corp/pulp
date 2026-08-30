/// @file capsule.cpp
/// Admission and export: the order of the steps is the security property.
///
/// Import reads the bounded manifest before it trusts anything, proves the
/// closure before it believes the manifest, recomputes the revision identity
/// before it verifies a signature against it, and extracts into a private
/// directory before any profile code looks at the result. Export runs the same
/// facts in reverse: it measures the bytes it was handed, publishes exactly
/// that closure, and writes once to a name that did not exist.

#include <pulp/authoring_capsule/capsule.hpp>

#include <pulp/authoring_capsule/safe_path.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::authoring_capsule {
namespace {

using runtime::Err;
using runtime::Ok;

CapsuleError make_error(CapsuleStatus status, std::string subject, std::string required = {},
                        std::string found = {}) {
    return CapsuleError{status, std::move(subject), std::move(required), std::move(found)};
}

/// A digest may be written with or without its `sha256:` algorithm label. The
/// label names the algorithm; it is not part of the identity. Comparing on the
/// body alone keeps a difference in spelling from being reported as a content
/// mismatch, which would send someone looking for tampering that never
/// happened.
std::string_view digest_body(std::string_view digest) {
    constexpr std::string_view label = "sha256:";
    if (digest.size() > label.size() && digest.substr(0, label.size()) == label)
        return digest.substr(label.size());
    return digest;
}

/// Severity order for the compatibility verdict. A capsule can miss several
/// requirements at once, so `unmet` reports the most severe one while
/// `capabilities` keeps the full per-capability detail. A capsule this runtime
/// cannot support at all is ranked above one that simply belongs to a sibling
/// product, because "you need X" is more actionable than "hand this to Y" when
/// both are true.
int verdict_severity(CompatibilityVerdict verdict) {
    switch (verdict) {
        case CompatibilityVerdict::supported: return 0;
        case CompatibilityVerdict::other_product: return 1;
        case CompatibilityVerdict::runtime_too_old: return 2;
        case CompatibilityVerdict::unsupported: return 3;
    }
    return 3;
}

/// Statuses a profile validator may legitimately return from a compatibility
/// check. One of these is preserved verbatim; anything else — including the
/// default-constructed `CapsuleError` a terse validator might return — is named
/// `runtime_floor_too_old`, because a generic admission failure is a defect.
bool is_compatibility_status(CapsuleStatus status) {
    switch (status) {
        case CapsuleStatus::unsupported_product:
        case CapsuleStatus::unsupported_profile:
        case CapsuleStatus::unsupported_profile_version:
        case CapsuleStatus::unsupported_capability:
        case CapsuleStatus::runtime_floor_too_old:
        case CapsuleStatus::schema_migration_refused: return true;
        default: return false;
    }
}

/// The same rule for the trust adapter: a verifier's own vocabulary survives,
/// and anything outside it resolves to `signature_invalid`.
bool is_trust_status(CapsuleStatus status) {
    switch (status) {
        case CapsuleStatus::signature_invalid:
        case CapsuleStatus::revoked_signer:
        case CapsuleStatus::downgrade_refused:
        case CapsuleStatus::creator_identity_required: return true;
        default: return false;
    }
}

/// Read the first signature attestation out of the manifest's descriptive
/// `attestations` array.
///
/// Attestations are excluded from the revision digest, which is what lets one
/// be attached to a capsule after its revision identity already exists — so
/// they travel as raw canonical JSON rather than as parsed fields. A row counts
/// as a signature when it carries a signer, an algorithm, and decodable base64
/// signature bytes; every other row is some other kind of attestation and is
/// skipped. `signed_payload_digest` is parsed so the caller can check that an
/// envelope naming a digest names the one computed from these bytes; the
/// verifier is handed that computed digest regardless.
std::optional<SignatureEnvelope> first_signature_envelope(std::string_view attestations_json) {
    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(attestations_json);
    } catch (...) {
        // An unreadable descriptive block is not a trust claim and cannot make
        // a capsule more trusted than one with no attestations at all, so it
        // yields no envelope rather than an admission failure.
        return std::nullopt;
    }

    if (!parsed.isArray()) return std::nullopt;

    for (std::uint32_t index = 0; index < parsed.size(); ++index) {
        const auto row = parsed[index];
        if (!row.isObject()) continue;

        const auto text = [&row](const char* key) -> std::string {
            const auto value = row[key];
            return value.isString() ? std::string(value.getString()) : std::string{};
        };

        const std::string signature_b64 = text("signature");
        SignatureEnvelope envelope;
        envelope.signer_id = text("signer_id");
        envelope.algorithm = text("algorithm");
        envelope.signed_payload_digest = text("signed_payload_digest");
        if (envelope.signer_id.empty() || envelope.algorithm.empty() || signature_b64.empty())
            continue;

        auto signature = runtime::base64_decode(signature_b64);
        if (!signature.has_value()) continue;
        envelope.signature = std::move(*signature);
        return envelope;
    }

    return std::nullopt;
}

void accumulate_rights(const ComponentPolicy& policy, RightsSummary& rights) {
    // `unknown` is recorded as `unknown`. It never decays to allowed here, and
    // no projection later in the pipeline can recover a permission this layer
    // refused to invent.
    if (policy.redistribution == Redistribution::unknown) rights.any_unknown_redistribution = true;
    if (policy.redistribution == Redistribution::restricted)
        rights.any_restricted_redistribution = true;
    if (policy.attribution_required) rights.attribution_required = true;
    if (!policy.license_expression.empty())
        rights.license_expressions.push_back(policy.license_expression);
}

/// A component blocks a self-contained redistributable claim when its bytes are
/// not in the capsule, or when the right to pass them on is anything other than
/// granted. An absent statement is not a permission, so `unknown` blocks.
bool blocks_self_contained(const ComponentPolicy& policy) {
    return policy.redistribution != Redistribution::allowed ||
           policy.source_availability != SourceAvailability::included;
}

void sort_unique(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

/// SHA-256 of a byte range, tolerating an empty range: `std::vector::data()`
/// may be null when the vector is empty, and a zero-length member is a legal
/// capsule member.
std::string digest_of(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::uint8_t kNone[1] = {0};
    const std::uint8_t* data = bytes.empty() ? kNone : bytes.data();
    return runtime::sha256_hex(data, bytes.size());
}

}  // namespace

runtime::Result<CapsulePreview, CapsuleError> preview_capsule(const CapsuleArchive& archive,
                                                              const ProfileRegistry& registry,
                                                              const AdmissionOptions& options) {
    // Preview executes nothing and reaches nothing. No capsule byte is run, no
    // profile code is loaded, no model or tool is invoked, no network request
    // is made, and nothing is written outside this process — not even a
    // temporary file. Everything below is derived from the bounded manifest
    // bytes plus the container shape the reader already validated. That is the
    // property the whole design rests on: a person can be shown what a capsule
    // from an untrusted machine claims to be, and decide, before consenting to
    // anything at all.

    // Step 1 — the bounded root manifest, and only the manifest.
    const auto manifest_span = archive.manifest_bytes();
    const std::string_view manifest_json(
        manifest_span.empty() ? "" : reinterpret_cast<const char*>(manifest_span.data()),
        manifest_span.size());

    auto parsed = parse_manifest(manifest_json);
    if (!parsed) return Err(std::move(parsed).error());

    CapsulePreview preview;
    preview.manifest = std::move(parsed).value();
    const Manifest& manifest = preview.manifest;

    // Step 2 — the closure, in both directions.
    //
    // The manifest is the closure's author, not one of its rows: a row for
    // `capsule.json` would have to carry the digest of the bytes that contain
    // it. It is therefore excluded from the closure on both sides, and a row
    // claiming it is itself a violation.
    const auto members = archive.members();

    std::unordered_map<std::string, std::uint64_t> archive_expanded;
    archive_expanded.reserve(members.size());
    for (const auto& member : members) {
        if (std::string_view(member.path) == kManifestPath) continue;
        archive_expanded.emplace(member.path, member.expanded_bytes);
    }

    std::unordered_set<std::string> declared_paths;
    declared_paths.reserve(manifest.files.size());
    for (const auto& entry : manifest.files) {
        // A manifest path is data from an untrusted machine even though the
        // container's own member names were already admitted, so it goes
        // through the same gate before it is compared or joined to anything.
        auto normalized = admit_member_path(entry.path, options.limits);
        if (!normalized) return Err(std::move(normalized).error());
        std::string path = std::move(normalized).value();

        if (std::string_view(path) == kManifestPath)
            return Err(make_error(CapsuleStatus::closure_violation, std::move(path)));

        // Two rows for one member would publish two policies for the same
        // bytes and leave the reader to choose between them.
        if (!declared_paths.insert(path).second)
            return Err(make_error(CapsuleStatus::closure_violation, std::move(path)));

        // Declared but absent: the capsule claims a component it did not ship.
        const auto found = archive_expanded.find(path);
        if (found == archive_expanded.end())
            return Err(make_error(CapsuleStatus::closure_violation, std::move(path)));

        // The declared size must match what the container says it will expand
        // to. The check is free from the central directory, and it is the only
        // thing that makes the preview's size figures trustworthy before a
        // single member has been inflated. A row that disagrees with its own
        // member is describing something other than what it shipped.
        if (found->second != entry.bytes)
            return Err(make_error(CapsuleStatus::closure_violation, path,
                                  std::to_string(entry.bytes),
                                  std::to_string(found->second)));
    }

    // Present but undeclared: a member smuggled past the closure the capsule
    // published. Walked in archive order so the reported path is deterministic.
    for (const auto& member : members) {
        if (std::string_view(member.path) == kManifestPath) continue;
        if (declared_paths.find(member.path) == declared_paths.end())
            return Err(make_error(CapsuleStatus::closure_violation, member.path));
    }

    // Step 3 — the revision identity, recomputed. Everything downstream that
    // binds to an identity binds to this one, never to the string the capsule
    // asserts about itself.
    auto recomputed = revision_digest(manifest);
    if (!recomputed) return Err(std::move(recomputed).error());
    const std::string computed_digest = std::move(recomputed).value();
    if (digest_body(computed_digest) != digest_body(manifest.revision_id))
        return Err(make_error(CapsuleStatus::digest_mismatch, "revision_id", computed_digest,
                              manifest.revision_id));

    // Steps 4 to 6 — what this runtime can offer. None of these is an
    // admission failure: the preview exists so a product can say which update,
    // which capability, or which sibling product the user needs. `unmet` starts
    // at `ok` so a caller reading it on a supported capsule sees "nothing
    // unmet" rather than a default status that looks like a stale rejection.
    preview.compatibility = CompatibilityVerdict::supported;
    preview.unmet = make_error(CapsuleStatus::ok, {});

    const auto note = [&preview](CompatibilityVerdict verdict, CapsuleError unmet) {
        if (verdict_severity(verdict) > verdict_severity(preview.compatibility)) {
            preview.compatibility = verdict;
            preview.unmet = std::move(unmet);
        }
    };

    // Step 4 — resolve the profile.
    const ProfileValidator* validator = registry.find(manifest.profile);
    if (validator == nullptr) {
        // The identifier and the version the capsule asked for, verbatim, so
        // the product can name the download rather than shrug.
        note(CompatibilityVerdict::unsupported,
             make_error(CapsuleStatus::unsupported_profile, manifest.profile,
                        std::to_string(manifest.profile_version), {}));
    } else {
        if (manifest.profile_version > validator->max_profile_version()) {
            note(CompatibilityVerdict::unsupported,
                 make_error(CapsuleStatus::unsupported_profile_version, manifest.profile,
                            std::to_string(manifest.profile_version),
                            std::to_string(validator->max_profile_version())));
        }

        // A capsule for a sibling product is understood, not rejected: the
        // caller can offer to hand it to the product that owns it. Coercing the
        // profile instead would silently reinterpret someone's work. An
        // unstated caller product cannot contradict the capsule's, so the
        // comparison is skipped rather than reported as a mismatch.
        if (!options.product.empty() && !manifest.product.empty() &&
            manifest.product != options.product) {
            note(CompatibilityVerdict::other_product,
                 make_error(CapsuleStatus::unsupported_product, manifest.product, manifest.product,
                            options.product));
        }
    }

    // Step 4b — the roles the profile requires must actually be present. The
    // profile publishes a role vocabulary precisely so this layer can check it
    // without understanding what any role means; leaving `required_roles()`
    // uncalled would make "an unknown required role fails closed" a promise
    // with no implementation behind it, and a capsule missing its graph or its
    // parameter map would reach `validate_staged` before anything noticed.
    //
    // Only the missing direction is checked here. A role the profile does not
    // list is not rejected: unknown *optional* metadata must round-trip, and
    // deciding which unknown roles matter is the profile's job, in
    // `validate_staged`.
    if (validator != nullptr) {
        std::unordered_set<std::string> present_roles;
        present_roles.reserve(manifest.files.size() + manifest.dependencies.size());
        for (const auto& entry : manifest.files) present_roles.insert(entry.role);
        for (const auto& entry : manifest.dependencies) present_roles.insert(entry.role);

        for (const auto& role : validator->required_roles()) {
            if (present_roles.find(role) == present_roles.end())
                note(CompatibilityVerdict::unsupported,
                     make_error(CapsuleStatus::missing_required_role, role, role, {}));
        }
    }

    // Step 5 — capabilities. Every requirement is reported, available or not,
    // so a caller never has to re-run the checks to explain a refusal.
    preview.capabilities.reserve(manifest.required_capabilities.size());
    for (const auto& name : manifest.required_capabilities) {
        const bool available = validator != nullptr && validator->supports_capability(name);
        preview.capabilities.push_back(CapabilityRequirement{name, available});
        if (!available)
            note(CompatibilityVerdict::unsupported,
                 make_error(CapsuleStatus::unsupported_capability, name));
    }

    // Step 6 — the runtime floor, answered by the profile that owns it.
    if (validator != nullptr) {
        auto compatible = validator->check_compatibility(manifest);
        if (!compatible) {
            CapsuleError unmet = std::move(compatible).error();
            if (!is_compatibility_status(unmet.status))
                unmet.status = CapsuleStatus::runtime_floor_too_old;
            note(CompatibilityVerdict::runtime_too_old, std::move(unmet));
        }
    }

    // Step 7 — trust. A signature answers *who*, and nothing else: it never
    // authorizes execution, never grants a redistribution right, and never
    // widens what the steps above concluded.
    if (const auto envelope = first_signature_envelope(manifest.attestations_json)) {
        preview.signer_id = envelope->signer_id;

        // An envelope that names a digest must name THIS one. Checked here as
        // well as inside the verifier, because a consumer's adapter is free to
        // ignore the field, and an envelope lifted wholesale from a different
        // capsule would otherwise reach that adapter looking ordinary.
        if (!envelope->signed_payload_digest.empty() &&
            digest_body(envelope->signed_payload_digest) != digest_body(computed_digest))
            return Err(make_error(CapsuleStatus::signature_invalid, envelope->signer_id,
                                  computed_digest, envelope->signed_payload_digest));

        if (options.verifier == nullptr) {
            // No verifier means no verification. A capsule that carries a
            // signature is admitted as UNSIGNED, because the absence of a
            // checker must never read as a passing check.
            preview.signature_verified = false;
        } else {
            // Verified against the digest computed from these bytes, never the
            // one the capsule asserts: a capsule allowed to nominate its own
            // digest could sign anything.
            auto verified = options.verifier->verify(*envelope, computed_digest);
            if (!verified) {
                CapsuleError error = std::move(verified).error();
                if (!is_trust_status(error.status)) error.status = CapsuleStatus::signature_invalid;
                if (error.subject.empty()) error.subject = envelope->signer_id;
                return Err(std::move(error));
            }

            // Distinct from an invalid signature on purpose: here the bytes are
            // intact and the identity is real, and which of those two failed is
            // exactly what the person needs to be told.
            if (options.verifier->is_revoked(envelope->signer_id))
                return Err(make_error(CapsuleStatus::revoked_signer, envelope->signer_id));

            VersionFloor floor;
            floor.product = manifest.product;
            floor.min_version = manifest.compatibility.min_product_version;
            auto within_floor = options.verifier->check_floor(floor);
            if (!within_floor) {
                CapsuleError error = std::move(within_floor).error();
                if (!is_trust_status(error.status)) error.status = CapsuleStatus::downgrade_refused;
                if (error.subject.empty()) error.subject = floor.product;
                if (error.required.empty()) error.required = floor.min_version;
                return Err(std::move(error));
            }

            preview.signature_verified = true;
        }
    }

    // Step 8 — the facts a person decides on.
    //
    // Completeness is recomputed from the component rows; a value the capsule
    // declared for itself is ignored.
    preview.completeness = derive_completeness(manifest);

    for (const auto& entry : manifest.files) {
        // Surfaced so consent is informed. It does not authorize execution:
        // admitting a capsule is not agreeing to build its source.
        if (entry.executable_data) preview.contains_executable_data = true;
        accumulate_rights(entry.policy, preview.rights);
        if (blocks_self_contained(entry.policy))
            preview.rights.blocking_component_paths.push_back(entry.path);
    }

    preview.dependencies.reserve(manifest.dependencies.size());
    for (const auto& dependency : manifest.dependencies) {
        DependencySummary summary;
        summary.id = dependency.id;
        summary.provider = dependency.provider;
        summary.license_expression = dependency.policy.license_expression;
        summary.redistribution = dependency.policy.redistribution;
        summary.required = dependency.required;
        // This layer owns no resolver and makes no network request, so it
        // cannot claim a dependency is locally resolvable. A consumer with a
        // content-addressed library answers this before showing the preview;
        // false is the honest default, and it is the safe one.
        summary.resolvable_locally = false;
        preview.dependencies.push_back(std::move(summary));

        accumulate_rights(dependency.policy, preview.rights);
        if (dependency.required && blocks_self_contained(dependency.policy))
            preview.rights.blocking_component_paths.push_back(dependency.id);
    }

    // Deterministic and deduplicated, so a difference between two previews is a
    // real difference in the capsules rather than in the order they were read.
    sort_unique(preview.rights.license_expressions);
    sort_unique(preview.rights.blocking_component_paths);

    // Sizes come from the container's directory, so no member is expanded to
    // report them. `archive_bytes` is the sum of the members' compressed
    // sizes and excludes the container's own framing, which makes it a lower
    // bound on the file size rather than a substitute for it.
    for (const auto& member : members) {
        preview.archive_bytes += member.compressed_bytes;
        preview.expanded_bytes += member.expanded_bytes;
    }
    preview.member_count = members.size();

    return Ok(std::move(preview));
}

runtime::Result<void, CapsuleError> admit_to_staging(const CapsuleArchive& archive,
                                                     const CapsulePreview& preview,
                                                     const ProfileRegistry& registry,
                                                     const StagingArea& staging,
                                                     const ExtractionProgress& progress) {
    const Manifest& manifest = preview.manifest;

    // A preview that could not be satisfied is a refusal, not advice. The
    // preview itself returns Ok for an unsupported capsule so a product can
    // explain what is missing; this is where that verdict has to bite. Leaving
    // it to the caller would mean the only thing between a capsule demanding a
    // capability this runtime lacks and a full extraction was somebody
    // remembering to read a field.
    //
    // `other_product` is deliberately not a refusal: it is understood, just not
    // ours, and the caller's whole job at that point is to hand it to the
    // product that owns it.
    if (preview.compatibility != CompatibilityVerdict::supported &&
        preview.compatibility != CompatibilityVerdict::other_product) {
        CapsuleError unmet = preview.unmet;
        // A preview built by hand could carry a non-verdict here; refuse with
        // something accurate rather than a status that reads like a stale
        // rejection.
        if (unmet.status == CapsuleStatus::ok)
            unmet = make_error(CapsuleStatus::unsupported_capability, manifest.profile);
        return Err(std::move(unmet));
    }

    const ProfileValidator* validator = registry.find(manifest.profile);
    if (validator == nullptr)
        return Err(make_error(CapsuleStatus::unsupported_profile, manifest.profile,
                              std::to_string(manifest.profile_version), {}));

    // Step 6 — only the members the manifest declares are written, each digest
    // verified as it lands, into a directory only this owner can read. An
    // undeclared member is never written at all, so a capsule cannot smuggle a
    // file past the closure it published.
    auto extracted = extract_declared(archive, manifest, staging, progress);
    if (!extracted) return Err(std::move(extracted).error());

    // Step 7 — the profile reads the staged tree. It does not publish and it
    // does not compile: publication is the caller's next step, and compiling
    // happens only after admission, inside the consumer's sandbox.
    auto validated = validator->validate_staged(manifest, staging.root());
    if (!validated) return Err(std::move(validated).error());

    return {};
}

runtime::Result<std::uint64_t, CapsuleError>
export_capsule(ExportRequest request, const std::filesystem::path& destination,
               const CapsuleLimits& limits) {
    // The inventory is explicit. Every byte that travels was handed to this
    // function by name; nothing here walks a directory, expands a glob, reads
    // an ignore file, or follows a link. That is what makes it structurally
    // impossible for an editor backup, a probe file, a build cache, a log, an
    // absolute path, or a credential to end up in a capsule: a file that was
    // not passed in cannot be exported by accident, so no export policy has to
    // remember to exclude it.

    // An exporter must not mint a capsule this same code would refuse to read.
    if (request.manifest.format != kFormatId)
        return Err(make_error(CapsuleStatus::unsupported_format, "format", std::string(kFormatId),
                              request.manifest.format));
    if (request.manifest.format_version != kFormatVersion)
        return Err(make_error(CapsuleStatus::unsupported_format_version, "format_version",
                              std::to_string(kFormatVersion),
                              std::to_string(request.manifest.format_version)));

    for (auto& item : request.items) {
        // The path is admitted before it is published, so a capsule cannot
        // carry a name the reader would have to reject or repair.
        auto normalized = admit_member_path(item.entry.path, limits);
        if (!normalized) return Err(std::move(normalized).error());
        item.entry.path = std::move(normalized).value();

        // `capsule.json` is the closure's author and cannot also be one of its
        // rows; an item claiming that name would collide with the manifest.
        if (std::string_view(item.entry.path) == kManifestPath)
            return Err(make_error(CapsuleStatus::closure_violation, item.entry.path));

        // Digest and size are measured from the bytes that actually travel,
        // never taken from what the caller declared about them. A closure is
        // only worth having if it was measured rather than copied.
        item.entry.sha256 = digest_of(item.bytes);
        item.entry.bytes = static_cast<std::uint64_t>(item.bytes.size());
    }

    // `files` is sorted by path in byte order and the archive is written in the
    // same order, so two exports of unchanged content agree on the revision
    // digest and on the archive bytes.
    std::sort(request.items.begin(), request.items.end(),
              [](const ExportItem& lhs, const ExportItem& rhs) {
                  return lhs.entry.path < rhs.entry.path;
              });

    std::vector<std::string> paths;
    paths.reserve(request.items.size());
    for (const auto& item : request.items) paths.push_back(item.entry.path);
    // Catches an exact duplicate and a pair that differs only by case or by a
    // confusable character — on the receiving machine one would overwrite the
    // other, and the closure would still look satisfied.
    if (auto collisions = check_collisions(paths); !collisions)
        return Err(std::move(collisions).error());

    request.manifest.files.clear();
    request.manifest.files.reserve(request.items.size());
    for (const auto& item : request.items) request.manifest.files.push_back(item.entry);

    // The digest excludes `revision_id` itself, so the identity is assigned
    // once the closure is complete and the manifest is otherwise final.
    auto digest = revision_digest(request.manifest);
    if (!digest) return Err(std::move(digest).error());
    request.manifest.revision_id = std::move(digest).value();

    auto canonical = to_canonical_json(request.manifest);
    if (!canonical) return Err(std::move(canonical).error());
    const std::string manifest_json = std::move(canonical).value();

    std::vector<WriteMember> members;
    members.reserve(request.items.size() + 1);
    // The manifest is member 0: a reader must be able to learn what a capsule
    // is from its first member, without scanning the rest of the container.
    members.push_back(WriteMember{
        std::string(kManifestPath),
        std::vector<std::uint8_t>(manifest_json.begin(), manifest_json.end())});
    for (auto& item : request.items)
        members.push_back(WriteMember{std::move(item.entry.path), std::move(item.bytes)});

    return write_archive_no_replace(members, destination, limits);
}

}  // namespace pulp::authoring_capsule
