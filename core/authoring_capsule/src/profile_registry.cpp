/// @file profile_registry.cpp
/// Profile lookup, and nothing more.
///
/// The registry is deliberately a map and not a policy: it stores what a
/// consumer registered and answers whether an identifier is present. It never
/// substitutes a near-match, never falls back to a "default" profile, and
/// never coerces a capsule into a profile that was not asked for. A missing
/// entry is returned as a null pointer so the caller can name the exact
/// identifier and version the capsule wanted, which is what lets a product
/// offer the right download instead of failing generically.

#include <pulp/authoring_capsule/profile_registry.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::authoring_capsule {

struct ProfileRegistry::Impl {
    /// Ordered by identifier, and keyed with a transparent comparator so a
    /// lookup by `string_view` does not have to allocate a key first. The
    /// ordering also makes `registered_profiles()` reproducible: the same set
    /// of registrations reports the same list regardless of the order they
    /// arrived in, so a diagnostic that lists the registry is comparable
    /// between two runs.
    std::map<std::string, std::shared_ptr<ProfileValidator>, std::less<>> validators;
};

ProfileRegistry::ProfileRegistry() : impl_(std::make_unique<Impl>()) {}
ProfileRegistry::~ProfileRegistry() = default;
ProfileRegistry::ProfileRegistry(ProfileRegistry&&) noexcept = default;
ProfileRegistry& ProfileRegistry::operator=(ProfileRegistry&&) noexcept = default;

void ProfileRegistry::register_profile(std::shared_ptr<ProfileValidator> validator) {
    // A moved-from registry holds no map, and a null validator has no
    // identifier to key on. Both are ignored rather than dereferenced: a
    // registry that crashes on a redundant registration is worse than one that
    // reports the profile as absent.
    if (impl_ == nullptr || validator == nullptr) return;

    std::string id(validator->profile_id());
    if (id.empty()) return;

    // A later registration replaces an earlier one for the same identifier, so
    // a consumer can install an updated validator without tearing the registry
    // down. Two validators for one identifier would otherwise leave which one
    // answers a lookup dependent on registration order.
    impl_->validators[std::move(id)] = std::move(validator);
}

const ProfileValidator* ProfileRegistry::find(std::string_view profile_id) const noexcept {
    if (impl_ == nullptr) return nullptr;
    const auto it = impl_->validators.find(profile_id);
    return it == impl_->validators.end() ? nullptr : it->second.get();
}

std::vector<std::string> ProfileRegistry::registered_profiles() const {
    std::vector<std::string> ids;
    if (impl_ == nullptr) return ids;
    ids.reserve(impl_->validators.size());
    for (const auto& [id, validator] : impl_->validators) ids.push_back(id);
    return ids;
}

}  // namespace pulp::authoring_capsule
