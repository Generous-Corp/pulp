#pragma once

/// Where a generation job runs, as a pure function of what the device can carry.
///
/// `pulp/platform/device_capability.hpp` says what the machine *is* — one tier
/// ladder, one thermal ladder. `pulp/format/device_quotas.hpp` says how much the
/// product grants at a rung. This header answers the remaining question: for a
/// job the product wants done, *where does it run*. Same address as the quota
/// table and for the same reason — this is policy, and policy does not belong in
/// the vocabulary header every module can reach.
///
/// Like the quota table, the policy is written out cell by cell rather than
/// derived from a scaling rule, so it is reviewable as data and so its
/// invariants are assertions about these cells rather than restatements of a
/// formula that generated them.
///
/// This header is constexpr and dependency-free beyond the capability
/// vocabulary. It selects a destination; it never performs, schedules, or bills
/// for the work, and it holds no notion of a model, a server, or a queue beyond
/// naming which of the three a job is sent to.

#include <pulp/platform/device_capability.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::format {

/// A kind of generation work, distinguished by what it costs and what it needs.
///
/// Three classes rather than one job type, because the offline and thermal
/// answers differ sharply between them: a proposal is expensive and deferrable,
/// playing an artifact that already exists is neither, and rendering a receipt
/// is cheap bookkeeping that must never be the thing that fails.
enum class GenerationJobClass : std::uint8_t {
    /// Producing a new proposal. The expensive class, and the only one that
    /// would ever want on-device inference.
    ProposalGeneration = 0,
    /// Playing an artifact that has already been materialized. No inference is
    /// involved; the work is decode and playback.
    ArtifactPlayback = 1,
    /// Rendering a receipt for work already done. Cheap, local, and required
    /// even when everything else is degraded — a user must always be able to
    /// see what was produced on their behalf.
    ReceiptRendering = 2,
};

inline constexpr std::size_t kGenerationJobClassCount = 3;

/// Where a job is sent.
enum class GenerationRoute : std::uint8_t {
    /// Runs locally on this device.
    OnDevice = 0,
    /// Sent to a server for execution now.
    Server = 1,
    /// Accepted and deferred; runs when connectivity or headroom returns.
    QueueForLater = 2,
};

inline constexpr std::size_t kGenerationRouteCount = 3;

/// The surface the product is running as.
///
/// This is not a device property, which is why it is not on the tier ladder: the
/// same phone is `Standalone` in the app and `AudioUnitV3` inside a host. The
/// distinction is load-bearing because an AUv3 runs inside someone else's
/// process under a memory limit it does not control, so it never gets to spend
/// that budget on inference.
enum class GenerationHostContext : std::uint8_t {
    /// Pulp's own application process.
    Standalone = 0,
    /// An AUv3 extension hosted by another application.
    AudioUnitV3 = 1,
};

/// Whether the device can reach a server right now.
enum class GenerationConnectivity : std::uint8_t {
    Online = 0,
    Offline = 1,
};

namespace detail {

/// Baseline route for `ProposalGeneration`, indexed `[tier][thermal state]`.
///
/// Only proposal generation needs a table: the other two classes have a single
/// answer each and stating them as tables would imply a variation that does not
/// exist. Playback and receipts are always local — they are what the product
/// falls back *to*, so routing them anywhere else would make degradation
/// depend on the very connectivity that degraded.
///
/// Two orderings hold across this table and are asserted in test: along a row,
/// the route never moves back toward the device as the machine gets hotter;
/// down a column, it never moves further from the device as the tier rises.
/// Together they say heat only ever pushes work away and capability only ever
/// pulls it closer, which is the whole intent of the policy in two sentences.
inline constexpr GenerationRoute
    kProposalRouteTable[platform::kDeviceCapabilityTierCount][platform::kThermalStateCount] = {
        // Constrained: never local. The rung is defined as review-and-perform
        // with no local realtime, so inference is out at every temperature.
        {GenerationRoute::Server, GenerationRoute::Server, GenerationRoute::Server,
         GenerationRoute::QueueForLater},
        // Standard: authoring-complete, but inference is not part of that
        // promise, so it offloads at every temperature it can reach a server at.
        {GenerationRoute::Server, GenerationRoute::Server, GenerationRoute::Server,
         GenerationRoute::QueueForLater},
        // Full: precision authoring with headroom — the only rung that spends
        // its own cycles on a proposal, and only while it is cool enough to.
        {GenerationRoute::OnDevice, GenerationRoute::Server, GenerationRoute::Server,
         GenerationRoute::QueueForLater},
};

} // namespace detail

/// Returns where `job` runs on a device at `tier` and `thermal`, running as
/// `context` with `connectivity`.
///
/// Total by construction: every combination has an answer, and the answer is
/// never "fail". Offline does not error — it degrades, which is why an offline
/// proposal is `QueueForLater` rather than a rejection, and why playback and
/// receipts are unaffected by it.
[[nodiscard]] constexpr GenerationRoute
generation_route(platform::DeviceCapabilityTier tier, platform::ThermalState thermal,
                 GenerationJobClass job, GenerationHostContext context,
                 GenerationConnectivity connectivity) noexcept {
    // Playing an existing artifact and rendering a receipt are local work that
    // needs no server and must survive every degradation. Answering them first
    // keeps them out of reach of the connectivity and thermal rules below.
    if (job != GenerationJobClass::ProposalGeneration) {
        return GenerationRoute::OnDevice;
    }

    // Offline outranks the table: with no server reachable, the only honest
    // answers are "run it here" or "hold it", and an AUv3 may not run it here.
    if (connectivity == GenerationConnectivity::Offline) {
        const auto local = tier == platform::DeviceCapabilityTier::Full &&
                           thermal == platform::ThermalState::Nominal &&
                           context == GenerationHostContext::Standalone;
        return local ? GenerationRoute::OnDevice : GenerationRoute::QueueForLater;
    }

    const auto tier_index = static_cast<std::size_t>(tier);
    const auto thermal_index = static_cast<std::size_t>(thermal);
    if (tier_index >= platform::kDeviceCapabilityTierCount ||
        thermal_index >= platform::kThermalStateCount) {
        // An out-of-ladder value is not a reason to run inference on a device
        // we cannot characterize; hold the job instead.
        return GenerationRoute::QueueForLater;
    }

    const auto route = detail::kProposalRouteTable[tier_index][thermal_index];

    // An AUv3 never spends its host's memory budget on inference, whatever the
    // table says. This is a hard substitution rather than a separate table so
    // that no future edit to the table can accidentally grant it.
    if (context == GenerationHostContext::AudioUnitV3 && route == GenerationRoute::OnDevice) {
        return GenerationRoute::Server;
    }
    return route;
}

/// Returns whether `route` performs inference on this device.
///
/// Named rather than left as a comparison so the AUv3 invariant reads as the
/// property it is at every call site that asserts it.
[[nodiscard]] constexpr bool runs_inference_on_device(GenerationJobClass job,
                                                      GenerationRoute route) noexcept {
    return job == GenerationJobClass::ProposalGeneration && route == GenerationRoute::OnDevice;
}

} // namespace pulp::format
