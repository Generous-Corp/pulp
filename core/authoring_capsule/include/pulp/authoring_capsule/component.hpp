#pragma once

/// @file component.hpp
/// Per-component policy. Rights, disclosure, and availability are properties
/// of each artifact, never of the capsule as a whole: a project may pair open
/// DSP with a proprietary UI, or a public patch with a sample the recipient
/// must supply. A project-wide `open_source` or `locked` flag cannot express
/// that, so this layer does not have one.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

enum class Canonicality : std::uint8_t {
    canonical_input,
    derived_output,
    preview,
    receipt,
};

enum class SourceAvailability : std::uint8_t {
    /// Bytes travel in the capsule.
    included,
    /// Bytes are named by digest and fetched from a declared provider.
    external,
    /// Bytes exist only on the exporting machine and were deliberately omitted.
    local_only,
    /// Deliberately absent under the selected export policy.
    omitted,
};

enum class Editability : std::uint8_t { editable, opaque };

enum class Disclosure : std::uint8_t {
    public_,
    recipient_scoped,
    private_,
    redacted,
    not_recorded,
};

/// `unknown` is a real state and never decays to `allowed`. An export policy
/// that claims redistribution must refuse rather than assume.
enum class Redistribution : std::uint8_t { allowed, restricted, unknown };

/// Operations a component is required for. A component missing from the
/// capsule downgrades exactly the operations that list it.
enum class RequiredFor : std::uint8_t { play, rebuild, remix, publish };

struct ComponentPolicy {
    Canonicality canonicality = Canonicality::canonical_input;
    SourceAvailability source_availability = SourceAvailability::included;
    Editability editability = Editability::editable;
    Disclosure disclosure = Disclosure::private_;
    Redistribution redistribution = Redistribution::unknown;
    /// SPDX expression, or a `LicenseRef-…` for a custom licence.
    std::string license_expression;
    /// Digest of the notice text that must travel with the component.
    std::string license_notice_sha256;
    std::string creator;
    std::string source_uri;
    bool attribution_required = false;
    std::vector<RequiredFor> required_for;
};

/// One row of the archive closure. Every archive member has exactly one of
/// these, and every one of these has exactly one archive member.
struct FileEntry {
    /// Profile-defined semantic role, e.g. `"dsp.source"`.
    std::string role;
    /// Normalized safe relative path inside the archive.
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
    /// True when the bytes are code the consuming product could execute. It
    /// does not authorize execution; it makes the preview honest.
    bool executable_data = false;
    ComponentPolicy policy;
};

/// A component whose bytes are not in the archive.
struct DependencyEntry {
    std::string role;
    std::string id;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
    /// Allowlisted HTTPS provider or content-addressed library locator. Never
    /// a path on the exporting machine.
    std::string provider;
    bool required = true;
    ComponentPolicy policy;
};

/// Derived from the component rows. A capsule that declares its own
/// completeness is ignored: the reader recomputes it.
enum class Completeness : std::uint8_t {
    /// Every required byte is present and may be redistributed.
    self_contained,
    /// Required components are absent but have stable identities and resolvers.
    resolvable,
    /// Inspectable or editable, but not fully rebuildable or playable.
    partial,
    /// The admitted rendition plays; canonical editable inputs are incomplete.
    play_only,
};

}  // namespace pulp::authoring_capsule
