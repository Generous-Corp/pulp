#pragma once

/// @file preview.hpp
/// The result of inspecting a capsule without running any of it.
///
/// Preview executes nothing: no JavaScript, no native code, no graph code, no
/// model tools, and no network. Everything below is derived from the bounded
/// manifest and the container shape.

#include <pulp/authoring_capsule/component.hpp>
#include <pulp/authoring_capsule/manifest.hpp>
#include <pulp/authoring_capsule/status.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

/// What the local runtime can offer for this capsule.
enum class CompatibilityVerdict : std::uint8_t {
    /// Profile registered, versions satisfied, capabilities available.
    supported,
    /// Understood, but this product is not its product. The caller can offer
    /// to hand it to the right one; it must never coerce the profile.
    other_product,
    /// Understood, but the local runtime is too old.
    runtime_too_old,
    /// The profile or a required capability is not registered here.
    unsupported,
};

struct CapabilityRequirement {
    std::string name;
    bool available = false;
};

struct DependencySummary {
    std::string id;
    std::string provider;
    std::string license_expression;
    Redistribution redistribution = Redistribution::unknown;
    bool required = true;
    bool resolvable_locally = false;
};

/// Rights facts, machine-derived. These are statements about what the capsule
/// declares, not a judgement: this layer never awards an "open source" badge
/// from file presence, and never invents permission a licence did not grant.
struct RightsSummary {
    bool any_unknown_redistribution = false;
    bool any_restricted_redistribution = false;
    bool attribution_required = false;
    std::vector<std::string> license_expressions;
    /// Components that block a self-contained redistributable claim.
    std::vector<std::string> blocking_component_paths;
};

struct CapsulePreview {
    Manifest manifest;
    CompatibilityVerdict compatibility = CompatibilityVerdict::unsupported;
    Completeness completeness = Completeness::partial;
    std::vector<CapabilityRequirement> capabilities;
    std::vector<DependencySummary> dependencies;
    RightsSummary rights;

    std::uint64_t archive_bytes = 0;
    std::uint64_t expanded_bytes = 0;
    std::size_t member_count = 0;

    /// True when at least one component carries `executable_data`. Surfaced so
    /// consent is informed: admitting a capsule is not the same as agreeing to
    /// build its source.
    bool contains_executable_data = false;

    /// True when a trust envelope was present and verified. A verified
    /// signature establishes identity, not permission to escape a sandbox.
    bool signature_verified = false;
    std::string signer_id;

    /// Populated when `compatibility` is not `supported`: the exact profile,
    /// version, capability, or runtime the user would need.
    CapsuleError unmet;
};

}  // namespace pulp::authoring_capsule
