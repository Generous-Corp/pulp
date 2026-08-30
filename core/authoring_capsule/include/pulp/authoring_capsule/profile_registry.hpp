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

    /// Roles this profile requires. A required role the reader does not know
    /// fails closed.
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
