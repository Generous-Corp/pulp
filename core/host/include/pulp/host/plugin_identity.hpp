// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace pulp::host {
namespace detail {
class ClapIdentityAccess;
}

/// CLAP-only type identity. Exact UTF-8 bytes; no name, path or normalization.
struct PluginTypeIdentity {
    std::string format = "clap";
    std::string descriptor_id;
    bool operator==(const PluginTypeIdentity&) const = default;
};

enum class PluginIdentityError {
    unsupported_format,
    native_identity_missing,
    invalid_native_identity,
    identity_origin_unverified,
    descriptor_selection_missing,
    descriptor_selection_ambiguous,
    descriptor_enumeration_incomplete,
    plugin_load_failed,
    loaded_identity_mismatch,
};

enum class PluginIdentityOrigin { authored_assertion, clap_descriptor };
enum class PluginIdentityVerification { declared, loaded };

/// Executable-byte observation, not a portable release identity or signature.
struct PluginArtifactProvenance {
    std::string sha256;
    std::uint64_t byte_length = 0;
    std::optional<std::string> descriptor_version;
    bool operator==(const PluginArtifactProvenance&) const = default;
};

/// Immutable owned evidence. Public imports can only make declared assertions.
/// Copies retain capture provenance; they do not attest to a current artifact.
class PluginIdentityEvidence {
  public:
    static PluginIdentityEvidence
    declared(PluginTypeIdentity type,
             PluginIdentityOrigin origin = PluginIdentityOrigin::authored_assertion,
             std::string source_contract = {}, PluginArtifactProvenance artifact = {}) {
        return {std::move(type), origin, std::move(source_contract), std::move(artifact),
                PluginIdentityVerification::declared};
    }
    const PluginTypeIdentity& type() const noexcept {
        return type_;
    }
    PluginIdentityOrigin origin() const noexcept {
        return origin_;
    }
    PluginIdentityVerification verification() const noexcept {
        return verification_;
    }
    const std::string& source_contract_revision() const noexcept {
        return source_contract_;
    }
    const PluginArtifactProvenance& artifact() const noexcept {
        return artifact_;
    }

  private:
    friend class detail::ClapIdentityAccess;
    PluginIdentityEvidence(PluginTypeIdentity type, PluginIdentityOrigin origin,
                           std::string contract, PluginArtifactProvenance artifact,
                           PluginIdentityVerification verification)
        : type_(std::move(type)), origin_(origin), source_contract_(std::move(contract)),
          artifact_(std::move(artifact)), verification_(verification) {}
    PluginTypeIdentity type_;
    PluginIdentityOrigin origin_;
    std::string source_contract_;
    PluginArtifactProvenance artifact_;
    PluginIdentityVerification verification_;
};

/// Pure validation: nonempty, bounded UTF-8 without NUL or Unicode controls.
std::optional<PluginIdentityError> validate_plugin_type_identity(const PluginTypeIdentity&);

struct CanonicalPluginIdentity {
    std::string bytes;
    PluginIdentityEvidence evidence;
};
/// Encodes only type identity; preserves evidence without increasing its trust.
std::variant<CanonicalPluginIdentity, PluginIdentityError>
canonical_plugin_identity(const PluginIdentityEvidence&);

/// Unsigned serialized captures always import as declared, even if originally loaded.
std::string serialize_plugin_identity_evidence(const PluginIdentityEvidence&);
std::optional<PluginIdentityEvidence> parse_plugin_identity_evidence(std::string_view);

struct PluginSelection {
    std::string path;
    PluginTypeIdentity type;
};
struct PluginResolvedIdentity {
    PluginTypeIdentity requested;
    PluginIdentityEvidence loaded;
};
} // namespace pulp::host
