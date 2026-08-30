#pragma once

/// @file profile_registry.hpp
/// Consumers register what a payload *means*. This layer never learns it.
///
/// The registry has no compile-time dependency on any consumer, which is what
/// keeps the substrate reusable: a second, genuinely different consumer must
/// fit without the substrate changing.

#include <pulp/authoring_capsule/manifest.hpp>
#include <pulp/authoring_capsule/preview.hpp>
#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

/// Semantic validation of an extracted staging tree. Runs after admission and
/// before publication, and must not execute any capsule content.
class ProfileValidator {
public:
    virtual ~ProfileValidator() = default;

    /// The profile this validator answers for, e.g.
    /// `"com.generous.forge.instrument"`.
    virtual std::string_view profile_id() const noexcept = 0;

    /// Highest profile version understood. A capsule above it is
    /// `unsupported_profile_version`; the caller reports the exact numbers so
    /// the user learns which update they need.
    virtual std::uint32_t max_profile_version() const noexcept = 0;

    /// Roles this profile requires.
    ///
    /// The substrate enforces this; a consumer must not repeat the check.
    /// `preview_capsule()` calls this and reports every role absent from the
    /// manifest's `files[]` and `dependencies[]` as `missing_required_role`,
    /// which lands as `CompatibilityVerdict::unsupported` in the preview
    /// rather than as an immediate error — the preview exists to tell a person
    /// what is missing. `admit_to_staging()` is where that verdict bites: it
    /// refuses before extracting anything, so nothing reaches
    /// `validate_staged()` with a required role missing.
    ///
    /// Only the missing direction is checked. A role this list does not name
    /// is not rejected: unknown optional metadata must round-trip, and which
    /// unrecognized roles matter is this profile's own judgment, in
    /// `validate_staged()`.
    virtual std::vector<std::string> required_roles() const = 0;

    /// Capabilities this profile can satisfy locally.
    virtual bool supports_capability(std::string_view name) const noexcept = 0;

    /// Is the local runtime new enough for this capsule?
    virtual runtime::Result<void, CapsuleError>
    check_compatibility(const Manifest& manifest) const = 0;

    /// Validate the extracted staging tree without executing it.
    virtual runtime::Result<void, CapsuleError>
    validate_staged(const Manifest& manifest, const std::filesystem::path& staging_root) const = 0;
};

class ProfileRegistry {
public:
    ProfileRegistry();
    ~ProfileRegistry();
    ProfileRegistry(ProfileRegistry&&) noexcept;
    ProfileRegistry& operator=(ProfileRegistry&&) noexcept;

    void register_profile(std::shared_ptr<ProfileValidator> validator);

    /// Null when the profile is not registered here. The caller turns that
    /// into `unsupported_profile` naming the exact identifier, so a product
    /// can offer the right download instead of failing generically.
    const ProfileValidator* find(std::string_view profile_id) const noexcept;

    std::vector<std::string> registered_profiles() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulp::authoring_capsule
