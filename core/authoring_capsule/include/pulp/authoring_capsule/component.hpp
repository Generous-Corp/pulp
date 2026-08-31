#pragma once

/// @file component.hpp
/// Per-component policy. Rights, disclosure, and availability are properties
/// of each artifact, never of the capsule as a whole: a project may pair open
/// DSP with a proprietary UI, or a public patch with a sample the recipient
/// must supply. A project-wide `open_source` or `locked` flag cannot express
/// that, so this layer does not have one.

#include <compare>
#include <cstdint>
#include <string>
#include <utility>
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

/// Permission to pass a component's bytes on.
///
/// This is a class rather than a plain enum because the permissive state must
/// be unreachable by accident. A default-constructed, value-initialized, or
/// zero-filled `Redistribution` is `unknown`; `allowed` can only be reached by
/// naming `Redistribution::granted()`, which reads as the act it is. That
/// matters because a grant is a statement about someone else's work: a row
/// whose author never spelled the grant must not acquire one from an aggregate
/// initializer, a `memset`, a forgotten field, or an enumerator that happened
/// to sit at zero. `unknown` is a real state and never decays to `allowed`;
/// an export policy that claims redistribution must refuse rather than assume.
class Redistribution {
public:
    /// `unknown` is first so that every zero-initialization lands on it.
    enum class State : std::uint8_t { unknown = 0, restricted = 1, allowed = 2 };

    /// The only implicit state. Silence is not a grant.
    constexpr Redistribution() = default;

    /// An explicit grant. Spelling this is the act; there is no other way in.
    [[nodiscard]] static constexpr Redistribution granted() {
        return Redistribution{State::allowed};
    }
    [[nodiscard]] static constexpr Redistribution restricted() {
        return Redistribution{State::restricted};
    }
    [[nodiscard]] static constexpr Redistribution unknown() { return Redistribution{}; }

    [[nodiscard]] constexpr State state() const { return state_; }
    [[nodiscard]] constexpr bool is_granted() const { return state_ == State::allowed; }
    [[nodiscard]] constexpr bool is_restricted() const { return state_ == State::restricted; }
    [[nodiscard]] constexpr bool is_unknown() const { return state_ == State::unknown; }

    friend constexpr bool operator==(const Redistribution&, const Redistribution&) = default;

private:
    constexpr explicit Redistribution(State state) : state_(state) {}

    State state_ = State::unknown;
};

/// Operations a component is required for. A component missing from the
/// capsule downgrades exactly the operations that list it.
enum class RequiredFor : std::uint8_t { play, rebuild, remix, publish };

struct ComponentPolicy {
    Canonicality canonicality = Canonicality::canonical_input;
    SourceAvailability source_availability = SourceAvailability::included;
    Editability editability = Editability::editable;
    Disclosure disclosure = Disclosure::private_;
    /// Defaults to `unknown` because `Redistribution` has no other default.
    Redistribution redistribution;
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

/// A reference to one component row, for a result that must name components
/// it does not own.
///
/// The two row kinds are identified in different namespaces: a `files[]` row
/// by its archive path, a `dependencies[]` row by its content identity. A
/// bare string cannot say which of the two a reader is holding, and a
/// path-only list cannot name a dependency at all — which would make such a
/// list structurally empty for a profile whose components are entirely
/// dependencies. Carrying the kind alongside the identifier is what keeps the
/// reference resolvable back to the row it came from.
struct ComponentRef {
    enum class Kind : std::uint8_t { file, dependency };

    Kind kind = Kind::file;
    /// The `files[]` row's archive path, or the `dependencies[]` row's `id`.
    std::string id;

    [[nodiscard]] static ComponentRef to_file(std::string path) {
        return ComponentRef{Kind::file, std::move(path)};
    }
    [[nodiscard]] static ComponentRef to_dependency(std::string id) {
        return ComponentRef{Kind::dependency, std::move(id)};
    }

    /// Ordered and compared by kind then identifier, so a list of these has a
    /// total order that does not depend on the order the rows were read.
    friend bool operator==(const ComponentRef&, const ComponentRef&) = default;
    friend std::strong_ordering operator<=>(const ComponentRef&, const ComponentRef&) = default;
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
