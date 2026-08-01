#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

/// Device capability vocabulary: one tier ladder, one thermal ladder, and the
/// projection from observed inputs onto them.
///
/// Everything here is an observation about the machine or a pure function of
/// observations. What the product then *grants* at a given rung is policy, and
/// policy is not here: `pulp/format/device_quotas.hpp` holds the quota table,
/// at a rung the engine modules cannot reach. `core/platform` is named in every
/// row of the engine's declared dependency floors, so anything placed here is
/// reachable by every module and no floor row can object to it — which makes it
/// the right address for a shared vocabulary and the wrong address for a
/// budget.
///
/// This header is deliberately dependency-free. It includes no Pulp header, so
/// it cannot name a document, a session, a transport, or a view, and every
/// entry point is a `constexpr` free function over by-value trivially-copyable
/// inputs. A projection that cannot reach mutable state by construction needs
/// no runtime guard against mutating one.
namespace pulp::platform {

// ── Capability tier ────────────────────────────────────────────────────────

/// How much of the product a device can carry, on one ascending ladder.
///
/// There is exactly one ladder. The browser lane's Tier A/B/C and the mobile
/// lane's M-A/M-B/M-C are the same three rungs named twice, so they are the
/// same three enumerators here:
///
/// | Rung          | Browser | Mobile | Shape                                 |
/// |---------------|---------|--------|---------------------------------------|
/// | `Constrained` | Tier A  | M-A    | Review and perform; no local realtime |
/// | `Standard`    | Tier B  | M-B    | Authoring-complete                    |
/// | `Full`        | Tier C  | M-C    | Precision authoring with headroom     |
///
/// The rung is computed from observable inputs (see `DeviceCapabilityInputs`),
/// never assumed from form factor: a tablet with a keyboard and a phone-sized
/// browser tab land wherever their capabilities put them.
enum class DeviceCapabilityTier : std::uint8_t {
    Constrained = 0,
    Standard = 1,
    Full = 2,
};

inline constexpr std::size_t kDeviceCapabilityTierCount = 3;

// ── Thermal ladder ─────────────────────────────────────────────────────────

/// Live thermal pressure, ascending in heat.
///
/// Four rungs, chosen to be the coarsest ladder every reporting platform can
/// be projected onto without inventing precision it does not have. Platforms
/// with a finer ladder collapse into these; a platform with no thermal API at
/// all can only ever report `Nominal`, which is why the absence of a thermal
/// API lowers the capability tier rather than being ignored — see
/// `DeviceCapabilityInputs::thermal_reporting_available`.
///
/// Consumers index a quota table by tier and thermal state together; the table
/// itself is `pulp::format::device_quotas`.
enum class ThermalState : std::uint8_t {
    Nominal = 0,
    Fair = 1,
    Serious = 2,
    Critical = 3,
};

inline constexpr std::size_t kThermalStateCount = 4;

// ── Observable inputs ──────────────────────────────────────────────────────

/// Precision of the device's primary pointing device.
enum class PointerPrecision : std::uint8_t {
    /// Touch or another pointer whose contact area exceeds a small target.
    Coarse = 0,
    /// Mouse, trackpad, or stylus with sub-target precision.
    Fine = 1,
};

/// What a shell observed about the device it is running on.
///
/// Every field is something a shell can measure. Nothing here is a form
/// factor, an OS name, or a browser name: a caller that knows only "this is a
/// phone" cannot fill this in, which is the point.
struct DeviceCapabilityInputs {
    /// Memory the process may actually use, not the memory the device has
    /// installed. A browser tab's budget and an app's jetsam limit both belong
    /// here rather than the hardware figure.
    std::uint64_t usable_memory_bytes = 0;

    /// Cores available to this process for sustained work.
    std::uint32_t usable_core_count = 0;

    /// Whether the platform will report thermal pressure at all. A device that
    /// cannot say it is hot cannot be trusted with the quotas that assume it
    /// will say so.
    bool thermal_reporting_available = false;

    /// Whether a realtime render path — a dedicated audio callback fed across
    /// a shared memory region — can be constructed at all.
    ///
    /// This is the neutral form of a lane-specific fact. A browser shell folds
    /// its own isolation, shared-buffer, and worklet-module observations into
    /// this one boolean before calling; a native shell sets it directly. The
    /// per-lane spellings stay in the shell that observed them, so this header
    /// never has to learn any of them.
    bool realtime_render_available = false;

    /// Precision of the primary pointer.
    PointerPrecision pointer_precision = PointerPrecision::Coarse;

    /// Whether a physical keyboard with modifier keys is attached.
    bool precision_keyboard_available = false;

    constexpr auto operator<=>(const DeviceCapabilityInputs&) const = default;
};

// ── Tier projection ────────────────────────────────────────────────────────

/// Memory floor for `DeviceCapabilityTier::Standard`.
inline constexpr std::uint64_t kStandardTierMemoryBytes = 3ull * 1024 * 1024 * 1024;
/// Memory floor for `DeviceCapabilityTier::Full`.
inline constexpr std::uint64_t kFullTierMemoryBytes = 8ull * 1024 * 1024 * 1024;
/// Core-count floor for `DeviceCapabilityTier::Standard`.
inline constexpr std::uint32_t kStandardTierCoreCount = 4;
/// Core-count floor for `DeviceCapabilityTier::Full`.
inline constexpr std::uint32_t kFullTierCoreCount = 8;

namespace detail {

/// Returns the lower of two tiers.
[[nodiscard]] constexpr DeviceCapabilityTier
min_tier(DeviceCapabilityTier left, DeviceCapabilityTier right) {
    return left < right ? left : right;
}

} // namespace detail

/// Projects observed capabilities onto the tier ladder.
///
/// Each axis independently states the highest rung it can support, and the
/// result is the lowest of those: capability is a weakest-link property, so a
/// device with sixteen cores and half a gigabyte is not a Full-tier device.
/// Structuring it this way keeps each axis's contribution separately
/// reviewable instead of hiding it in a cascade of conditions.
[[nodiscard]] constexpr DeviceCapabilityTier
project_device_capability_tier(DeviceCapabilityInputs inputs) {
    const DeviceCapabilityTier by_memory =
        inputs.usable_memory_bytes >= kFullTierMemoryBytes      ? DeviceCapabilityTier::Full
        : inputs.usable_memory_bytes >= kStandardTierMemoryBytes ? DeviceCapabilityTier::Standard
                                                                 : DeviceCapabilityTier::Constrained;

    const DeviceCapabilityTier by_cores =
        inputs.usable_core_count >= kFullTierCoreCount      ? DeviceCapabilityTier::Full
        : inputs.usable_core_count >= kStandardTierCoreCount ? DeviceCapabilityTier::Standard
                                                             : DeviceCapabilityTier::Constrained;

    // No realtime render path caps the device at the review-and-perform rung,
    // whatever its memory and cores: the rungs above it are defined by being
    // able to render locally.
    const DeviceCapabilityTier by_realtime = inputs.realtime_render_available
                                                 ? DeviceCapabilityTier::Full
                                                 : DeviceCapabilityTier::Constrained;

    // Full-tier quotas assume the system will report heat so the consumer can
    // step down. Without that signal the top rung is not grantable.
    const DeviceCapabilityTier by_thermal_reporting = inputs.thermal_reporting_available
                                                          ? DeviceCapabilityTier::Full
                                                          : DeviceCapabilityTier::Standard;

    // The top rung is precision authoring, which presumes a precise pointer
    // and modifier keys. Neither bars authoring outright, so each caps at
    // Standard rather than Constrained.
    const DeviceCapabilityTier by_pointer = inputs.pointer_precision == PointerPrecision::Fine
                                                ? DeviceCapabilityTier::Full
                                                : DeviceCapabilityTier::Standard;
    const DeviceCapabilityTier by_keyboard = inputs.precision_keyboard_available
                                                 ? DeviceCapabilityTier::Full
                                                 : DeviceCapabilityTier::Standard;

    DeviceCapabilityTier tier = by_memory;
    tier = detail::min_tier(tier, by_cores);
    tier = detail::min_tier(tier, by_realtime);
    tier = detail::min_tier(tier, by_thermal_reporting);
    tier = detail::min_tier(tier, by_pointer);
    tier = detail::min_tier(tier, by_keyboard);
    return tier;
}

} // namespace pulp::platform
