#pragma once

#include "detail/shared_io_provider_identity.hpp"

#include <string_view>

namespace pulp::test {

// The verdict of a physical-Dawn qualification once its own evidence is in.
// Failed evidence is a failure whatever the identity says. With the evidence
// sound, an unauthenticated provider is a failure, and a provider whose
// native runtime has no independent identity source is a typed skip (exit 77
// under --strict), never a pass and never a failure.
enum class PhysicalQualificationOutcome {
    Completed,
    Failed,
    Unavailable,
};

enum class PhysicalQualificationAdmission {
    Allowed,
    Failed,
    Unavailable,
};

inline PhysicalQualificationAdmission physical_qualification_admission(
    pulp::gpu_audio::detail::SharedIoProviderIdentityGate gate) noexcept {
    using Gate = pulp::gpu_audio::detail::SharedIoProviderIdentityGate;
    if (gate == Gate::Unauthenticated)
        return PhysicalQualificationAdmission::Failed;
    if (gate == Gate::NativeRuntimeUnavailable)
        return PhysicalQualificationAdmission::Unavailable;
    return PhysicalQualificationAdmission::Allowed;
}

template <typename Arm>
PhysicalQualificationAdmission
arm_physical_qualification_trial(pulp::gpu_audio::detail::SharedIoProviderIdentityGate gate,
                                 Arm&& arm) {
    const auto admission = physical_qualification_admission(gate);
    if (admission != PhysicalQualificationAdmission::Allowed)
        return admission;
    return arm() ? PhysicalQualificationAdmission::Allowed : PhysicalQualificationAdmission::Failed;
}

inline PhysicalQualificationOutcome physical_qualification_outcome(
    bool evidence_ok, pulp::gpu_audio::detail::SharedIoProviderIdentityGate gate) noexcept {
    using Gate = pulp::gpu_audio::detail::SharedIoProviderIdentityGate;
    if (!evidence_ok || gate == Gate::Unauthenticated)
        return PhysicalQualificationOutcome::Failed;
    return gate == Gate::NativeRuntimeUnavailable ? PhysicalQualificationOutcome::Unavailable
                                                  : PhysicalQualificationOutcome::Completed;
}

inline constexpr std::string_view kNativeRuntimeIdentityUnavailable =
    "native_runtime_identity_unavailable";

} // namespace pulp::test
